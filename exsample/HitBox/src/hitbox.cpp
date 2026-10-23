#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <bedrock/game_api.hpp>
#include <bedrock/game_hooks.hpp>
#include <bedrock/gui_overlay.hpp>
#include <bedrock/logger.hpp>
#include <bedrock/native_actors.hpp>
#include <bedrock/native_items.hpp>
#include <bedrock/remote_catalog.hpp>
#include <bedrock/resolver.hpp>
#include <entt/entt.hpp>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hitbox_native {
struct EntityId {
    std::uint32_t rawId{};
    [[nodiscard]] constexpr bool operator==(const EntityId&) const = default;
    [[nodiscard]] constexpr operator std::uint32_t() const { return rawId; }
};
struct EntityIdTraits {
    using value_type = EntityId;
    using entity_type = std::uint32_t;
    using version_type = std::uint16_t;
    static constexpr entity_type entity_mask = 0x3FFFF;
    static constexpr version_type version_mask = 0x3FFF;
};
}

namespace entt {
template<> struct entt_traits<hitbox_native::EntityId>
    : basic_entt_traits<hitbox_native::EntityIdTraits> {
    static constexpr std::size_t page_size = 2048;
};
}

namespace {
enum class Status : unsigned { NotStarted, Loading, Running, Failed, Stopped };
using Clock = std::chrono::steady_clock;

struct Config {
    bool enabled = true;
    bool debug = false;
    bool retainLastPosition = false;
    unsigned retentionMinutes = 10;
    float nearbyDistance = 32.0f;
    float lineThickness = 1.5f;
    ImU32 liveColor = IM_COL32(76, 230, 132, 230);
    ImU32 retainedColor = IM_COL32(255, 174, 66, 210);
};

struct BoxRecord {
    std::uintptr_t actorKey{};
    bedrock::ActorBounds bounds;
    Clock::time_point seenAt;
};
struct RenderSnapshot {
    std::vector<BoxRecord> boxes;
    bedrock::Vec3 cameraPosition;
    bedrock::ActorRotation cameraRotation;
    float fovDegrees = 70.0f;
};

struct NativeActorViewStats {
    bool requiredStorageAvailable{};
    std::size_t ownerStorage{};
    std::size_t actorTypeStorage{};
    bool actorTypeAvailable{};
    std::size_t runtimeIdStorage{};
    std::size_t aabbStorage{};
    std::size_t iterated{};
    std::size_t liveEntities{};
    std::size_t viewMatches{};
    std::size_t validActorPointers{};
};

std::atomic<Status> status{Status::NotStarted};
std::atomic<bool> stopRequested{false};
// Use the shared_ptr atomic free functions for cross-standard-library
// compatibility. Some C++20 libraries do not yet implement atomic<shared_ptr>.
std::shared_ptr<const Config> liveConfig{std::make_shared<const Config>()};
std::shared_ptr<const RenderSnapshot> renderSnapshot{
    std::make_shared<const RenderSnapshot>()};
std::atomic<float> currentFov{70.0f};
std::mutex logMutex;
std::ofstream logFile;
std::unordered_map<std::uintptr_t, BoxRecord> records;
std::unordered_set<std::uint32_t> loggedActorIds;
std::unordered_map<std::uint32_t, std::uint64_t> pendingActorNameLogs;
std::atomic<void*> currentWorld{};

template<class... Args>
void log(Args&&... args) {
    std::lock_guard lock(logMutex);
    if (!logFile) return;
    (logFile << ... << std::forward<Args>(args));
    logFile << '\n';
    logFile.flush();
}

std::filesystem::path dataDirectory() {
    const auto environmentPath = [](const wchar_t* name) -> std::wstring {
        const auto length = GetEnvironmentVariableW(name, nullptr, 0);
        if (length <= 1) return {};
        std::vector<wchar_t> value(length);
        const auto written = GetEnvironmentVariableW(name, value.data(), length);
        if (written == 0 || written >= length) return {};
        return std::wstring(value.data(), written);
    };
    const auto linuxPath = [](std::wstring value) {
        if (!value.empty() && value.front() == L'/') {
            std::replace(value.begin(), value.end(), L'/', L'\\');
            value.insert(0, L"Z:"); // Proton/Wine maps the host root to Z:\\.
        }
        return std::filesystem::path{value};
    };

    if (const auto overridePath = environmentPath(L"BEDROCK_HITBOX_DIR");
        !overridePath.empty())
        return linuxPath(overridePath);

    const auto home = environmentPath(L"HOME");
    const auto prefix = environmentPath(L"WINEPREFIX");
    if (!home.empty() && home.front() == L'/' && !prefix.empty())
        return linuxPath(home) / L"Documents" / L"PEXData" / L"HitBox";

    PWSTR documents = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &documents)))
        return {};
    std::filesystem::path result{documents};
    CoTaskMemFree(documents);
    return result / L"PEXData" / L"HitBox";
}

nlohmann::json configJson(const Config& config) {
    const auto colorString = [](ImU32 color) {
        std::array<char, 10> text{};
        std::snprintf(text.data(), text.size(), "#%02X%02X%02X%02X",
            color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF, (color >> 24) & 0xFF);
        return std::string{text.data()};
    };
    return nlohmann::json{{"enabled", config.enabled},
        {"debug", config.debug},
        {"nearby_distance", config.nearbyDistance},
        {"retain_last_position", config.retainLastPosition},
        {"retention_minutes", config.retentionMinutes},
        {"line_thickness", config.lineThickness},
        {"live_color", colorString(config.liveColor)},
        {"retained_color", colorString(config.retainedColor)}};
}

Config parseConfig(const nlohmann::json& root) {
    Config config;
    config.enabled = root.value("enabled", config.enabled);
    config.debug = root.value("debug", config.debug);
    config.nearbyDistance = std::clamp(root.value("nearby_distance", config.nearbyDistance), 4.0f, 128.0f);
    config.retainLastPosition = root.value("retain_last_position", config.retainLastPosition);
    config.retentionMinutes = std::clamp(root.value("retention_minutes", config.retentionMinutes), 1u, 60u);
    config.lineThickness = std::clamp(root.value("line_thickness", config.lineThickness), 0.5f, 5.0f);
    const auto parseColor = [&](const char* key, ImU32 fallback) {
        const auto it = root.find(key);
        if (it == root.end() || !it->is_string()) return fallback;
        const auto value = it->get<std::string>();
        if ((value.size() != 7 && value.size() != 9) || value.front() != '#') return fallback;
        try {
            const auto r = std::stoul(value.substr(1, 2), nullptr, 16);
            const auto g = std::stoul(value.substr(3, 2), nullptr, 16);
            const auto b = std::stoul(value.substr(5, 2), nullptr, 16);
            const auto a = value.size() == 9 ? std::stoul(value.substr(7, 2), nullptr, 16) : 255ul;
            return IM_COL32(r, g, b, a);
        } catch (...) { return fallback; }
    };
    config.liveColor = parseColor("live_color", config.liveColor);
    config.retainedColor = parseColor("retained_color", config.retainedColor);
    return config;
}

void loadConfigFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return;
    try {
        const auto json = nlohmann::json::parse(input);
        std::atomic_store_explicit(&liveConfig,
            std::make_shared<const Config>(parseConfig(json)), std::memory_order_release);
    } catch (const std::exception& error) {
        log("config ignored: ", error.what());
    }
}

template<class T>
bool safeRead(const void* base, std::size_t offset, T& output) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    if (!base) return false;
    MEMORY_BASIC_INFORMATION info{};
    const auto* address = static_cast<const std::byte*>(base) + offset;
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    __try { output = *reinterpret_cast<const T*>(address); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool isExecutable(const void* address) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT)
        return false;
    const auto protection = info.Protect & 0xFF;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

bool looksLikeActor(void* actor) noexcept {
    void** vtable = nullptr;
    if (!safeRead(actor, 0, vtable) || !vtable) return false;
    unsigned valid = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        void* entry = nullptr;
        if (safeRead(vtable, i * sizeof(void*), entry) && isExecutable(entry)) ++valid;
    }
    return valid >= 2;
}

class HitBoxAccess final : public bedrock::GameAccess {
public:
    void setLayouts(bedrock::NativeItemLayout items, bedrock::NativeActorLayout actors) {
        itemLayout_ = std::move(items);
        actorLayout_ = std::move(actors);
    }
    void setClient(void* client) noexcept { client_.store(client, std::memory_order_release); }
    void setWorld(void* world) noexcept { world_.store(world, std::memory_order_release); }

    bool isActorValid(void* actor) const noexcept override { return looksLikeActor(actor); }
    void* localPlayer() const noexcept override {
        void* client = client_.load(std::memory_order_acquire);
        if (!client || itemLayout_.localPlayerVtableSlot > 0x1000) return nullptr;
        void** vtable = nullptr;
        void* target = nullptr;
        if (!safeRead(client, 0, vtable) || !safeRead(vtable,
                itemLayout_.localPlayerVtableSlot * sizeof(void*), target) || !isExecutable(target))
            return nullptr;
        return callLocalPlayer(target, client);
    }
    void* worldOf(void* actor) const noexcept override {
        return isActorValid(actor) ? world_.load(std::memory_order_acquire) : nullptr;
    }
    std::optional<std::string> nameTag(void*) const override { return std::nullopt; }
    std::vector<std::string> playerListNames(void*) const override { return {}; }
    std::optional<bedrock::ServerAddress> serverAddress(void*) const override {
        return std::nullopt;
    }
    std::vector<void*> actorHandles(void* world) const override {
        std::vector<void*> result;
        if (!world || world != world_.load(std::memory_order_acquire)) return result;
        auto* local = localPlayer();
        void* registry = registryFor(local);
        if (!registry) return result;
        NativeActorViewStats stats{};
        const bool enumerated = enumerateActorView(registry, actorLayout_, this,
                                                   &result, &stats);
        if (!nativeViewStatsLogged_.exchange(true, std::memory_order_relaxed)) {
            log("native Actor EnTT view: owner=", stats.ownerStorage,
                " actor_type=", stats.actorTypeStorage,
                stats.actorTypeAvailable ? "" : " [ActorType storage missing; using validated 3-component fallback]",
                " runtime_id=", stats.runtimeIdStorage,
                " aabb=", stats.aabbStorage,
                " iterated=", stats.iterated,
                " live=", stats.liveEntities,
                " view_matches=", stats.viewMatches,
                " valid_actor_ptrs=", stats.validActorPointers,
                " result=", result.size(),
                enumerated ? " [ok]" : stats.requiredStorageAvailable
                    ? " [view rejected by validation]" : " [required storage missing]");
        }
        if (!enumerated) result.clear();
        return result;
    }
    std::optional<bedrock::ActorBounds> actorBounds(void* actor) const override {
        bedrock::ActorBounds result{};
        return readActorBounds(actor, actorLayout_, &result) && result.valid()
            ? std::optional<bedrock::ActorBounds>{result} : std::nullopt;
    }
    std::optional<bedrock::Vec3> actorPosition(void* actor) const override {
        bedrock::Vec3 result{};
        return readComponentVec3(actor, actorLayout_, actorLayout_.stateVectorComponentHash,
            actorLayout_.stateVectorComponentHashAlternate, actorLayout_.statePositionOffset,
            &result) ? std::optional<bedrock::Vec3>{result} : std::nullopt;
    }
    std::optional<bedrock::ActorRotation> actorRotation(void* actor) const override {
        bedrock::ActorRotation result{};
        return readActorRotation(actor, actorLayout_, &result)
            ? std::optional<bedrock::ActorRotation>{result} : std::nullopt;
    }
    std::optional<std::uint32_t> actorEntityId(void* actor) const override {
        std::uint32_t entityId = 0;
        const auto offset = actorLayout_.actorEntityContextOffset +
                            actorLayout_.contextEntityIdOffset;
        return isActorValid(actor) && safeRead(actor, offset, entityId)
            ? std::optional<std::uint32_t>{entityId} : std::nullopt;
    }

private:
    static void* callLocalPlayer(void* target, void* client) noexcept {
        using Function = void*(__fastcall*)(void*);
        __try {
            auto* player = reinterpret_cast<Function>(target)(client);
            return looksLikeActor(player) ? player : nullptr;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    void* registryFor(void* actor) const noexcept {
        void* registry = nullptr;
        const auto offset = actorLayout_.actorEntityContextOffset +
                            actorLayout_.contextRegistryOffset;
        if (!isActorValid(actor) || !safeRead(actor, offset, registry)) return nullptr;
        return registry;
    }
    static bool enumerateActorView(void* registry, const bedrock::NativeActorLayout& layout,
        const HitBoxAccess* access, std::vector<void*>* output,
        NativeActorViewStats* stats) noexcept {
        if (!registry || !access || !output || !stats) return false;
        __try {
            using Registry = entt::basic_registry<hitbox_native::EntityId>;
            auto* typed = static_cast<Registry*>(registry);
            auto* owner = typed->storage(layout.actorOwnerComponentHash);
            auto* actorType = typed->storage(layout.actorTypeComponentHash);
            auto* runtimeId = typed->storage(layout.runtimeIdComponentHash);
            auto* aabb = typed->storage(layout.aabbShapeComponentHash);
            stats->requiredStorageAvailable = owner && runtimeId && aabb;
            if (!owner || !runtimeId || !aabb) return false;

            stats->ownerStorage = owner->size();
            stats->actorTypeAvailable = actorType != nullptr;
            stats->actorTypeStorage = actorType ? actorType->size() : 0;
            stats->runtimeIdStorage = runtimeId->size();
            stats->aabbStorage = aabb->size();

            // This is the runtime equivalent of
            // view<ActorOwner, ActorType, RuntimeID, AABBShape>().each().
            // Iterate the smallest pool and test membership in the other
            // three pools, avoiding a temporary EnTT view and reducing work.
            auto* candidate = owner;
            if (actorType && actorType->size() < candidate->size()) candidate = actorType;
            if (runtimeId->size() < candidate->size()) candidate = runtimeId;
            if (aabb->size() < candidate->size()) candidate = aabb;

            for (const auto entity : *candidate) {
                ++stats->iterated;
                if (!typed->valid(entity)) continue;
                ++stats->liveEntities;
                if (!owner->contains(entity) || (actorType && !actorType->contains(entity)) ||
                    !runtimeId->contains(entity) || !aabb->contains(entity)) continue;
                ++stats->viewMatches;

                void* ownerComponent = owner->value(entity);
                void* actor = nullptr;
                if (!ownerComponent ||
                    !safeRead(ownerComponent, layout.actorOwnerPointerOffset, actor) ||
                    !access->isActorValid(actor)) continue;

                // The owner row must resolve back to this exact registry
                // entity. This rejects stale owner pointers and coincidental
                // component membership without dereferencing actor inventory.
                void* actorRegistry = nullptr;
                std::uint32_t actorEntity = 0;
                const auto context = layout.actorEntityContextOffset;
                if (!safeRead(actor, context + layout.contextRegistryOffset, actorRegistry) ||
                    actorRegistry != registry ||
                    !safeRead(actor, context + layout.contextEntityIdOffset, actorEntity) ||
                    actorEntity != static_cast<std::uint32_t>(entity)) continue;
                ++stats->validActorPointers;
                output->push_back(actor);
            }
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static void* component(void* actor, const bedrock::NativeActorLayout& layout,
        std::uint32_t hash, std::uint32_t alternate) noexcept {
        void* registry = nullptr;
        std::uint32_t rawId = 0;
        const auto registryOffset = layout.actorEntityContextOffset + layout.contextRegistryOffset;
        const auto entityIdOffset = layout.actorEntityContextOffset + layout.contextEntityIdOffset;
        if (!actor || !safeRead(actor, registryOffset, registry) || !registry ||
            !safeRead(actor, entityIdOffset, rawId)) return nullptr;
        __try {
            using Registry = entt::basic_registry<hitbox_native::EntityId>;
            auto* typed = static_cast<Registry*>(registry);
            auto* storage = typed->storage(hash);
            hitbox_native::EntityId entity{rawId};
            if ((!storage || !storage->contains(entity)) && alternate) {
                storage = typed->storage(alternate);
            }
            if (!storage || !storage->contains(entity)) return nullptr;
            return storage->value(entity);
        } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    static bool readActorBounds(void* actor, const bedrock::NativeActorLayout& layout,
                                bedrock::ActorBounds* output) noexcept {
        if (!output) return false;
        void* shape = component(actor, layout, layout.aabbShapeComponentHash,
                                layout.aabbShapeComponentHashAlternate);
        if (!shape || !safeRead(shape, layout.aabbMinimumOffset, output->minimum) ||
            !safeRead(shape, layout.aabbMaximumOffset, output->maximum) || !output->valid())
            return false;
        const float width = output->maximum.x - output->minimum.x;
        const float height = output->maximum.y - output->minimum.y;
        const float depth = output->maximum.z - output->minimum.z;
        return width > 0.0f && width <= 64.0f && height > 0.0f && height <= 64.0f &&
               depth > 0.0f && depth <= 64.0f;
    }
    static bool readComponentVec3(void* actor, const bedrock::NativeActorLayout& layout,
        std::uint32_t hash, std::uint32_t alternate, std::size_t offset,
        bedrock::Vec3* output) noexcept {
        if (!output) return false;
        void* value = component(actor, layout, hash, alternate);
        return value && safeRead(value, offset, *output) &&
               std::isfinite(output->x) && std::isfinite(output->y) && std::isfinite(output->z);
    }
    static bool readActorRotation(void* actor, const bedrock::NativeActorLayout& layout,
                                  bedrock::ActorRotation* output) noexcept {
        if (!output) return false;
        void* rotation = component(actor, layout, layout.actorRotationComponentHash,
                                   layout.actorRotationComponentHashAlternate);
        return rotation && safeRead(rotation, layout.rotationPitchOffset, output->pitch) &&
               safeRead(rotation, layout.rotationYawOffset, output->yaw) &&
               std::isfinite(output->pitch) && std::isfinite(output->yaw);
    }

    bedrock::NativeItemLayout itemLayout_{};
    bedrock::NativeActorLayout actorLayout_{};
    std::atomic<void*> client_{};
    std::atomic<void*> world_{};
    mutable std::atomic<bool> nativeViewStatsLogged_{};
};

HitBoxAccess access;
bedrock::GameApi gameApi{access};
std::unique_ptr<bedrock::GameHooks> gameHooks;

float distanceSquared(const bedrock::Vec3& a, const bedrock::Vec3& b) {
    const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return x*x + y*y + z*z;
}

void observeWorld(const bedrock::LevelTickEvent& event) {
    if (event.level) currentWorld.store(event.level, std::memory_order_release);
}

void refreshFrameSnapshot() {
    static bool loggedActorCount = false;
    static bool loggedCameraFailure = false;
    static std::uint64_t frameSequence = 0;
    static void* snapshotWorld = nullptr;
    static std::vector<bedrock::Actor> actorRoster;
    static Clock::time_point actorRosterUpdated{};
    static bool actorRosterInitialized = false;
    ++frameSequence;
    auto config = std::atomic_load_explicit(&liveConfig, std::memory_order_acquire);
    if (!config->enabled) return;
    auto* world = currentWorld.load(std::memory_order_acquire);
    if (!world) return;
    if (snapshotWorld != world) {
        snapshotWorld = world;
        access.setWorld(world);
        records.clear();
        actorRoster.clear();
        actorRosterUpdated = {};
        actorRosterInitialized = false;
        loggedActorIds.clear();
        pendingActorNameLogs.clear();
        gameApi.clearObservedActors();
        std::atomic_store_explicit(&renderSnapshot,
            std::make_shared<const RenderSnapshot>(), std::memory_order_release);
        log("world actor source ready; retained hitboxes cleared for new world");
    }

    auto local = gameApi.localPlayer();
    const auto cameraPosition = local.getPosition();
    const auto cameraRotation = local.getRotation();
    if (!cameraPosition || !cameraRotation) {
        if (!loggedCameraFailure) {
            loggedCameraFailure = true;
            log("waiting for native local Actor position/rotation components");
        }
        return;
    }

    // Present/Execute can run many times per game tick. Full native EnTT
    // enumeration belongs on a bounded cadence, not on every GPU frame.
    // Positions and bounds are still sampled each frame from this handle cache
    // so the visible boxes remain smooth.
    const auto now = Clock::now();
    if (!actorRosterInitialized || now - actorRosterUpdated >= std::chrono::milliseconds(250)) {
        actorRoster = gameApi.actors(world);
        actorRosterUpdated = now;
        actorRosterInitialized = true;
    }

    const float range2 = config->nearbyDistance * config->nearbyDistance;
    std::unordered_set<std::uintptr_t> seen;
    const auto& actors = actorRoster;
    seen.reserve(actors.size());
    for (const auto& actor : actors) {
        const auto position = actor.getPosition();
        if (!position || distanceSquared(*position, *cameraPosition) > range2) continue;
        const auto bounds = actor.getBounds();
        if (!bounds) continue;
        const auto key = reinterpret_cast<std::uintptr_t>(actor.handle());
        if (!key) continue;
        seen.insert(key);
        records.insert_or_assign(key, BoxRecord{key, *bounds, now});
        if (config->debug) {
            const auto entityId = actor.getEntityId();
            if (entityId && !loggedActorIds.contains(*entityId)) {
                const auto name = actor.getName();
                if (!name.empty()) {
                    log("debug Actor detected: entity_id=", *entityId, " name=", name);
                    loggedActorIds.insert(*entityId);
                    pendingActorNameLogs.erase(*entityId);
                } else {
                    const auto [pending, inserted] = pendingActorNameLogs.emplace(
                        *entityId, frameSequence);
                    if (!inserted && frameSequence - pending->second >= 60) {
                        log("debug Actor detected: entity_id=", *entityId,
                            " name=<unavailable>");
                        loggedActorIds.insert(*entityId);
                        pendingActorNameLogs.erase(pending);
                    }
                }
            }
        }
    }
    if (!loggedActorCount && (!actors.empty() || frameSequence > 120)) {
        loggedActorCount = true;
        log("native Actor snapshot: actor_count=", actors.size(),
            " nearby_hitboxes=", seen.size());
    }

    const auto expiry = now - std::chrono::minutes(config->retentionMinutes);
    for (auto it = records.begin(); it != records.end();) {
        if ((!config->retainLastPosition && !seen.contains(it->first)) || it->second.seenAt < expiry)
            it = records.erase(it);
        else ++it;
    }

    auto snapshot = std::make_shared<RenderSnapshot>();
    snapshot->boxes.reserve(records.size());
    for (const auto& [_, box] : records) snapshot->boxes.push_back(box);
    snapshot->cameraPosition = *cameraPosition;
    snapshot->cameraRotation = *cameraRotation;
    snapshot->fovDegrees = currentFov.load(std::memory_order_acquire);
    std::shared_ptr<const RenderSnapshot> immutableSnapshot = std::move(snapshot);
    std::atomic_store_explicit(&renderSnapshot, std::move(immutableSnapshot),
                               std::memory_order_release);
}

std::optional<ImVec2> project(const bedrock::Vec3& point, const RenderSnapshot& view,
                              ImVec2 viewport) {
    constexpr float radians = 0.01745329251994329577f;
    const float yaw = view.cameraRotation.yaw * radians;
    const float pitch = view.cameraRotation.pitch * radians;
    const float sy = std::sin(yaw), cy = std::cos(yaw);
    const float sp = std::sin(pitch), cp = std::cos(pitch);
    const float dx = point.x - view.cameraPosition.x;
    const float dy = point.y - (view.cameraPosition.y + 1.62f);
    const float dz = point.z - view.cameraPosition.z;
    const float right = dx * cy + dz * sy;
    const float up = dx * (-sy * sp) + dy * cp + dz * (cy * sp);
    const float depth = dx * (-sy * cp) + dy * (-sp) + dz * (cy * cp);
    if (!std::isfinite(depth) || depth < 0.05f) return std::nullopt;
    const float fov = std::clamp(view.fovDegrees, 30.0f, 150.0f) * radians;
    const float focal = viewport.y / (2.0f * std::tan(fov * 0.5f));
    const ImVec2 result{viewport.x * 0.5f + right * focal / depth,
                        viewport.y * 0.5f - up * focal / depth};
    if (!std::isfinite(result.x) || !std::isfinite(result.y)) return std::nullopt;
    return result;
}

void drawBoxes(void*) {
    const auto config = std::atomic_load_explicit(&liveConfig, std::memory_order_acquire);
    if (!config->enabled) return;
    try {
        refreshFrameSnapshot();
    } catch (...) {
        static std::atomic<bool> loggedFailure{false};
        if (!loggedFailure.exchange(true, std::memory_order_relaxed))
            log("frame Actor refresh failed; retaining previous render snapshot");
    }
    const auto snapshot = std::atomic_load_explicit(&renderSnapshot, std::memory_order_acquire);
    if (!snapshot || snapshot->boxes.empty()) return;
    const ImVec2 viewport = ImGui::GetIO().DisplaySize;
    auto* draw = ImGui::GetBackgroundDrawList();
    constexpr std::array<std::array<unsigned, 2>, 12> edges{{
        {{0,1}},{{1,2}},{{2,3}},{{3,0}},{{4,5}},{{5,6}},{{6,7}},{{7,4}},
        {{0,4}},{{1,5}},{{2,6}},{{3,7}}}};
    const auto now = Clock::now();
    for (const auto& box : snapshot->boxes) {
        const std::array<bedrock::Vec3, 8> corners{{
            {box.bounds.minimum.x, box.bounds.minimum.y, box.bounds.minimum.z},
            {box.bounds.maximum.x, box.bounds.minimum.y, box.bounds.minimum.z},
            {box.bounds.maximum.x, box.bounds.minimum.y, box.bounds.maximum.z},
            {box.bounds.minimum.x, box.bounds.minimum.y, box.bounds.maximum.z},
            {box.bounds.minimum.x, box.bounds.maximum.y, box.bounds.minimum.z},
            {box.bounds.maximum.x, box.bounds.maximum.y, box.bounds.minimum.z},
            {box.bounds.maximum.x, box.bounds.maximum.y, box.bounds.maximum.z},
            {box.bounds.minimum.x, box.bounds.maximum.y, box.bounds.maximum.z}}};
        std::array<std::optional<ImVec2>, 8> screen;
        for (std::size_t i = 0; i < corners.size(); ++i)
            screen[i] = project(corners[i], *snapshot, viewport);
        const bool retained = now - box.seenAt >= std::chrono::milliseconds(500);
        const ImU32 color = retained ? config->retainedColor : config->liveColor;
        for (const auto& edge : edges) {
            const auto& a = screen[edge[0]];
            const auto& b = screen[edge[1]];
            if (a && b) draw->AddLine(*a, *b, color, config->lineThickness);
        }
    }
}

std::filesystem::path configPath() { return dataDirectory() / L"config.json"; }

DWORD WINAPI startClient(void*) {
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&startClient), &pinned)) {
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    status.store(Status::Loading, std::memory_order_release);
    const auto directory = dataDirectory();
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) { status.store(Status::Failed, std::memory_order_release); return 1; }
    logFile.open(directory / L"HitBox.log", std::ios::binary | std::ios::trunc);
    log("start");
    const auto cfgPath = configPath();
    if (!std::filesystem::exists(cfgPath)) {
        std::ofstream output(cfgPath, std::ios::binary | std::ios::trunc);
        output << configJson(Config{}).dump(2) << '\n';
    }
    loadConfigFile(cfgPath);
    log("config ready");

    bedrock::ProfileChoice choice;
    std::string profile, error;
    if (!bedrock::fetchSupportedProfile(profile, choice, error,
            [](std::string_view stage) { log("profile ", stage); })) {
        log("profile unavailable: ", error);
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    bedrock::Resolver resolver;
    bedrock::NativeItemLayout itemLayout;
    bedrock::NativeActorLayout actorLayout;
    if (!resolver.loadJson(profile, error) ||
        !resolver.attachWindowsMainModule(choice.identity, error) || !resolver.exactBuild() ||
        !bedrock::parseNativeItemLayout(profile, itemLayout, error) ||
        !bedrock::parseNativeActorLayout(profile, actorLayout, error)) {
        log("exact-build native Actor profile unavailable: ", error);
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    access.setLayouts(std::move(itemLayout), std::move(actorLayout));
    log("native Actor registry source ready [profile candidate; fail-closed reads]");

    gameHooks = std::make_unique<bedrock::GameHooks>(gameApi);
    gameApi.Event.Enum.Gameplay.then([](const bedrock::GameplayEvent& event) {
        if (event.clientInstance) access.setClient(event.clientInstance);
    });
    gameApi.Event.Enum.Fov.then([](const bedrock::FovEvent& event) {
        currentFov.store(event.inputFov, std::memory_order_release);
    });
    gameApi.Event.Enum.LevelTick.then(&observeWorld);
    if (!gameHooks->installActorNameTag(resolver, error) ||
        !gameHooks->installGameplay(resolver, error) ||
        !gameHooks->installFov(resolver, error) ||
        !gameHooks->installLevelTick(resolver, error)) {
        log("SDK hook registration failed: ", error);
        gameHooks.reset();
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    bedrock::gui::OverlayOptions options;
    options.captureInput = false;
    options.nativeCursor = false;
    options.initiallyVisible = true;
    if (!bedrock::gui::Overlay::install(&drawBoxes, nullptr, error, options)) {
        log("overlay install failed: ", error);
        gameHooks.reset();
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    log("ready ", choice.version, " [native Actors]");
    status.store(Status::Running, std::memory_order_release);

    auto lastWrite = std::filesystem::last_write_time(cfgPath, ec);
    while (!stopRequested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const auto write = std::filesystem::last_write_time(cfgPath, ec);
        if (!ec && write != lastWrite) {
            loadConfigFile(cfgPath);
            lastWrite = write;
        }
    }
    bedrock::gui::Overlay::shutdown();
    gameHooks.reset();
    status.store(Status::Stopped, std::memory_order_release);
    return 0;
}
}

extern "C" __declspec(dllexport) unsigned HitBox_GetStatus() {
    return static_cast<unsigned>(status.load(std::memory_order_acquire));
}
extern "C" __declspec(dllexport) void HitBox_Stop() {
    stopRequested.store(true, std::memory_order_release);
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, &startClient, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
        else status.store(Status::Failed, std::memory_order_release);
    } else if (reason == DLL_PROCESS_DETACH) {
        stopRequested.store(true, std::memory_order_release);
    }
    return TRUE;
}
#endif
