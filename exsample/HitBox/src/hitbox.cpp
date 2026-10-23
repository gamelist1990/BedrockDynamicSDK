#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <bedrock/game_api.hpp>
#include <bedrock/game_hooks.hpp>
#include <bedrock/hook_runtime.h>
#include <bedrock/logger.hpp>
#include <bedrock/native_actors.hpp>
#include <bedrock/native_items.hpp>
#include <bedrock/remote_catalog.hpp>
#include <bedrock/resolver.hpp>
#include <bedrock/generated_symbols.hpp>
#include <entt/entt.hpp>

#include <nlohmann/json.hpp>
#include "hitbox/box_cache.hpp"
#include "hitbox/world_render_profile.hpp"
#include <condition_variable>
#include <deque>

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

constexpr std::uint32_t packColor(unsigned r,unsigned g,unsigned b,unsigned a) {
    return r|(g<<8)|(b<<16)|(a<<24);
}
struct Config {
    bool enabled = true;
    bool debug = false;
    bool retainLastPosition = false;
    unsigned retentionMinutes = 10;
    float nearbyDistance = 32.0f;
    float lineThickness = 1.5f;
    std::uint32_t liveColor = packColor(76, 230, 132, 230);
    std::uint32_t retainedColor = packColor(255, 174, 66, 210);
};

using BoxRecord = hitbox::BoxRecord;
struct CameraState {
    std::uint64_t epoch{};
    bedrock::Vec3 cameraPosition{};
};
struct RenderSnapshot {
    std::uint64_t epoch{};
    std::vector<BoxRecord> boxes;
    std::chrono::milliseconds liveLifetime{1000};
};
constexpr std::size_t workerCount = 2;
constexpr std::size_t maximumNativeBatch = 8;
struct CaptureBatch {
    CameraState camera;
    std::array<hitbox::Sample, maximumNativeBatch> samples;
    std::size_t count{};
    std::size_t population{};
    std::chrono::milliseconds liveLifetime{1000};
    std::shared_ptr<const Config> config;
};
struct ProcessingWorker {
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<CaptureBatch> pending; // Fixed latest-input slot, no unbounded backlog.
    std::thread thread;
    bool stopping{};
    std::shared_ptr<const RenderSnapshot> snapshot;
};
std::array<ProcessingWorker, workerCount> processingWorkers;
std::atomic<std::uint64_t> worldEpoch{};
std::atomic<std::int64_t> gameHeartbeatMs{};
std::atomic<Status> status{Status::NotStarted};
std::atomic<bool> stopRequested{false};
// Use the shared_ptr atomic free functions for cross-standard-library
// compatibility. Some C++20 libraries do not yet implement atomic<shared_ptr>.
std::shared_ptr<const Config> liveConfig{std::make_shared<const Config>()};
// Logs are drained by a third persistent worker, never flushed by game/render callbacks.
std::mutex logMutex;
std::condition_variable logReady;
std::deque<std::string> logQueue;
std::thread logThread;
bool logStopping{};
std::atomic<void*> currentWorld{};

template<class... Args>
void log(Args&&... args) noexcept {
    try {
        std::ostringstream text;
        (text << ... << std::forward<Args>(args));
        std::unique_lock lock(logMutex, std::try_to_lock);
        if (lock && logQueue.size() < 512) logQueue.push_back(std::move(text).str());
    } catch (...) {}
}

void startLogWorker(const std::filesystem::path& path) {
    logThread = std::thread([path] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        try {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            for (;;) {
                std::deque<std::string> batch;
                bool done;
                {
                    std::unique_lock lock(logMutex);
                    logReady.wait_for(lock, std::chrono::milliseconds(250), [] { return logStopping; });
                    batch.swap(logQueue);
                    done = logStopping;
                }
                for (const auto& line : batch) file << line << '\n';
                if (!batch.empty()) file.flush();
                if (done) return;
            }
        } catch (...) {}
    });
}
void stopLogWorker() noexcept {
    { std::lock_guard lock(logMutex); logStopping = true; }
    logReady.notify_one();
    if (logThread.joinable()) logThread.join();
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
    const auto colorString = [](std::uint32_t color) {
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
    const auto parseColor = [&](const char* key, std::uint32_t fallback) {
        const auto it = root.find(key);
        if (it == root.end() || !it->is_string()) return fallback;
        const auto value = it->get<std::string>();
        if ((value.size() != 7 && value.size() != 9) || value.front() != '#') return fallback;
        try {
            const auto r = std::stoul(value.substr(1, 2), nullptr, 16);
            const auto g = std::stoul(value.substr(3, 2), nullptr, 16);
            const auto b = std::stoul(value.substr(5, 2), nullptr, 16);
            const auto a = value.size() == 9 ? std::stoul(value.substr(7, 2), nullptr, 16) : 255ul;
            return packColor(r, g, b, a);
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

// Keep VirtualQuery results only within one native callback. Scalar reads still
// have their SEH boundary; no native address validation is reused on later ticks.
struct NativeReadWindow {
    struct Region { std::uintptr_t begin{}, end{}; DWORD protection{}; };
    std::array<Region, 16> regions{};
    std::size_t count{};
    NativeReadWindow* previous{};
    static thread_local NativeReadWindow* active;
    NativeReadWindow() noexcept : previous(active) { active = this; }
    ~NativeReadWindow() { active = previous; }
};
thread_local NativeReadWindow* NativeReadWindow::active{};

bool permittedAddress(const void* pointer, std::size_t length, bool code = false, bool write = false) noexcept {
    const auto start = reinterpret_cast<std::uintptr_t>(pointer);
    if (!start || !length || start > UINTPTR_MAX - length) return false;
    const auto end = start + length;
    const auto permitted = [code, write](DWORD protection) {
        if (write) return protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        if (code) return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        return protection == PAGE_READONLY || protection == PAGE_READWRITE ||
            protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    };
    auto* window = NativeReadWindow::active;
    if (window) {
        for (std::size_t i = 0; i < window->count; ++i) {
            const auto& region = window->regions[i];
            if (start >= region.begin && end <= region.end) return permitted(region.protection);
        }
    }
    auto cursor = start;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const auto protection = info.Protect & 0xFF;
        if (!permitted(protection)) return false;
        const auto begin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (begin > UINTPTR_MAX - info.RegionSize) return false;
        const auto regionEnd = begin + info.RegionSize;
        if (regionEnd <= cursor) return false;
        if (window && window->count < window->regions.size())
            window->regions[window->count++] = {begin, regionEnd, protection};
        cursor = regionEnd;
    }
    return true;
}

template<class T>
bool safeRead(const void* base, std::size_t offset, T& output) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    if (!start || start > UINTPTR_MAX - offset) return false;
    const auto* address = reinterpret_cast<const T*>(start + offset);
    if (!permittedAddress(address, sizeof(T))) return false;
    __try { output = *address; return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool isExecutable(const void* address) noexcept { return permittedAddress(address, 1, true); }

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
        void* world = world_.load(std::memory_order_acquire);
        if (!client || itemLayout_.localPlayerVtableSlot > 0x1000) return nullptr;
        struct LocalCache {
            const HitBoxAccess* adapter{};
            void* client{};
            void* world{};
            void* actor{};
            void* registry{};
            std::uint32_t entity{};
            Clock::time_point expires{};
        };
        static thread_local LocalCache cached;
        void* registry = nullptr;
        std::uint32_t entity = 0;
        const auto context = actorLayout_.actorEntityContextOffset;
        if (cached.adapter == this && cached.client == client && cached.world == world && cached.actor && Clock::now() < cached.expires &&
            safeRead(cached.actor, context + actorLayout_.contextRegistryOffset, registry) &&
            registry == cached.registry &&
            safeRead(cached.actor, context + actorLayout_.contextEntityIdOffset, entity) &&
            entity == cached.entity) return cached.actor;
        cached = {};
        void** vtable = nullptr;
        void* target = nullptr;
        if (!safeRead(client, 0, vtable) || !safeRead(vtable,
                itemLayout_.localPlayerVtableSlot * sizeof(void*), target) || !isExecutable(target))
            return nullptr;
        void* actor = callLocalPlayer(target, client);
        if (actor && safeRead(actor, context + actorLayout_.contextRegistryOffset, registry) && registry &&
            safeRead(actor, context + actorLayout_.contextEntityIdOffset, entity))
            cached = {this, client, world, actor, registry, entity, Clock::now()+std::chrono::milliseconds(500)};
        return actor;
    }

    void* worldOf(void* actor) const noexcept override {
        return isActorValid(actor) ? world_.load(std::memory_order_acquire) : nullptr;
    }
    std::optional<std::string> nameTag(void*) const override { return std::nullopt; }
    std::vector<std::string> playerListNames(void*) const override { return {}; }
    std::optional<bedrock::ServerAddress> serverAddress(void*) const override {
        return std::nullopt;
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

    bool captureBatch(void* local, std::size_t& cursor, CaptureBatch& batch,
                      std::size_t& visited) const noexcept {
        // No EnTT iterator/storage/component pointer survives the callback.
        visited = 0;
        void* registry = nullptr;
        const auto& layout = actorLayout_;
        if (!safeRead(local, layout.actorEntityContextOffset + layout.contextRegistryOffset,
                      registry) || !registry) return false;
        return captureRegistryBatch(registry, layout, &cursor, &batch, &visited);
    }
private:
    static bool captureRegistryBatch(void* registry, const bedrock::NativeActorLayout& layout,
        std::size_t* cursor, CaptureBatch* batch, std::size_t* visited) noexcept {
        __try {
            using Registry = entt::basic_registry<hitbox_native::EntityId>;
            auto* typed = static_cast<Registry*>(registry);
            auto* owner = typed->storage(layout.actorOwnerComponentHash);
            auto* runtime = typed->storage(layout.runtimeIdComponentHash);
            auto* shape = typed->storage(layout.aabbShapeComponentHash);
            auto* actorType = typed->storage(layout.actorTypeComponentHash);
            if (!owner || !runtime || !shape) return false;
            auto* candidates = owner;
            // A fixed source keeps the incremental cursor stable when pool sizes change.
            batch->population = candidates->size();
            if (batch->population > 16384) return false;
            if (!batch->population) { *cursor = 0; return true; }
            const auto begin = Clock::now();
            const auto count = std::min(maximumNativeBatch, batch->population);
            for (std::size_t n = 0; n < count; ++n) {
                // Cooperative 250us budget; a single native read cannot be preempted.
                if (n && Clock::now() - begin >= std::chrono::microseconds(250)) break;
                if (*cursor >= batch->population) *cursor = 0;
                const auto entity = candidates->data()[(*cursor)++];
                ++*visited;
                if (!typed->valid(entity) || !owner->contains(entity) ||
                    !runtime->contains(entity) || !shape->contains(entity) ||
                    (actorType && !actorType->contains(entity))) continue;
                // Validate the owner against the exact registry/entity once, rather
                // than validating the Actor vtable for every property wrapper.
                void* actor = nullptr;
                if (!safeRead(owner->value(entity), layout.actorOwnerPointerOffset, actor) || !actor)
                    continue;
                void* actorRegistry = nullptr;
                std::uint32_t actorEntity = 0;
                const auto context = layout.actorEntityContextOffset;
                if (!safeRead(actor, context + layout.contextRegistryOffset, actorRegistry) ||
                    actorRegistry != registry ||
                    !safeRead(actor, context + layout.contextEntityIdOffset, actorEntity) ||
                    actorEntity != static_cast<std::uint32_t>(entity)) continue;
                auto& sample = batch->samples[batch->count];
                if (!safeRead(runtime->value(entity), layout.runtimeIdValueOffset, sample.runtimeId) ||
                    !sample.runtimeId) continue;
                auto* aabb = shape->value(entity);
                if (layout.aabbMaximumOffset == layout.aabbMinimumOffset + sizeof(bedrock::Vec3)) {
                    if (!safeRead(aabb, layout.aabbMinimumOffset, sample.bounds)) continue;
                } else if (!safeRead(aabb, layout.aabbMinimumOffset, sample.bounds.minimum) ||
                           !safeRead(aabb, layout.aabbMaximumOffset, sample.bounds.maximum)) continue;
                if (!sample.bounds.valid()) continue;
                const auto& b = sample.bounds;
                if (b.maximum.x - b.minimum.x <= 0 || b.maximum.x - b.minimum.x > 64 ||
                    b.maximum.y - b.minimum.y <= 0 || b.maximum.y - b.minimum.y > 64 ||
                    b.maximum.z - b.minimum.z <= 0 || b.maximum.z - b.minimum.z > 64) continue;
                sample.seenAt = Clock::now();
                ++batch->count;
            }
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) { batch->count = 0; return false; }
    }

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
};

HitBoxAccess access;
bedrock::GameApi gameApi{access};
std::unique_ptr<bedrock::GameHooks> gameHooks;

void processingLoop(std::size_t shard) noexcept {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    auto& worker = processingWorkers[shard];
    hitbox::BoxCache cache;
    std::uint64_t loggedEpoch = 0;
    Clock::time_point nextLog{};
    // Retain replaced snapshots until no callback holds them. Their vectors
    // are then destroyed here, instead of on the game's/renderer's last release.
    std::vector<std::shared_ptr<const RenderSnapshot>> retired;
    for (;;) {
        std::optional<CaptureBatch> batch;
        {
            std::unique_lock lock(worker.mutex);
            worker.ready.wait_for(lock, std::chrono::milliseconds(250), [&] {
                return worker.stopping || worker.pending.has_value();
            });
            if (worker.stopping) return;
            batch = std::move(worker.pending);
            worker.pending.reset();
        }
        try {
            std::erase_if(retired, [](const auto& value) { return value.use_count() == 1; });
            if (!batch || batch->camera.epoch != worldEpoch.load(std::memory_order_acquire) ||
                !cache.beginWorld(batch->camera.epoch)) continue;
            const auto& config = *batch->config;
            if (loggedEpoch != batch->camera.epoch) { loggedEpoch = batch->camera.epoch; nextLog = {}; }
            for (std::size_t i = 0; i < batch->count; ++i) {
                const auto& sample = batch->samples[i];
                if (sample.runtimeId % workerCount != shard) continue;
                cache.observe(sample, batch->camera.cameraPosition,
                              config.nearbyDistance, config.retainLastPosition);
            }
            auto snapshot = std::make_shared<RenderSnapshot>();
            snapshot->epoch = batch->camera.epoch;
            snapshot->liveLifetime = batch->liveLifetime;
            snapshot->boxes = cache.visible(Clock::now(), batch->camera.cameraPosition,
                config.nearbyDistance, config.retainLastPosition,
                config.retentionMinutes, batch->liveLifetime);
            if (snapshot->epoch != worldEpoch.load(std::memory_order_acquire)) continue;
            const auto previous = std::atomic_load_explicit(&worker.snapshot, std::memory_order_acquire);
            if (previous) retired.push_back(previous);
            std::atomic_store_explicit(&worker.snapshot,
                std::shared_ptr<const RenderSnapshot>(std::move(snapshot)), std::memory_order_release);
            if (config.debug && Clock::now() >= nextLog) {
                nextLog = Clock::now() + std::chrono::seconds(10);
                log("HitBox worker ", shard, ": native_pool=", batch->population,
                    " copied=", batch->count, " cached=", cache.size(), " [RuntimeID partition]");
            }
        } catch (...) {}
    }
}
void stopProcessingWorkers() noexcept {
    for (auto& worker : processingWorkers) {
        { std::lock_guard lock(worker.mutex); worker.stopping = true; worker.pending.reset(); }
        worker.ready.notify_one();
    }
    for (auto& worker : processingWorkers) if (worker.thread.joinable()) worker.thread.join();
}
bool startProcessingWorkers() noexcept {
    try {
        for (std::size_t i = 0; i < workerCount; ++i)
            processingWorkers[i].thread = std::thread(processingLoop, i);
        return true;
    } catch (...) { stopProcessingWorkers(); return false; }
}
void refreshGameTickSnapshot() {
    static void* snapshotWorld = nullptr;
    static std::size_t cursor = 0;
    static auto nextCapture = Clock::time_point{};
    static auto nextReport = Clock::time_point{};
    static double averageVisited = maximumNativeBatch;
    static bool loggedNativeReady = false;
    static bool loggedCameraWaiting = false;
    static bool loggedNativeFailure = false;
    auto config = std::atomic_load_explicit(&liveConfig, std::memory_order_acquire);
    if (!config->enabled || stopRequested.load(std::memory_order_acquire)) return;
    auto* world = currentWorld.load(std::memory_order_acquire);
    if (!world) return;
    const auto now = Clock::now();
    if (snapshotWorld != world) {
        snapshotWorld = world;
        access.setWorld(world);
        worldEpoch.fetch_add(1, std::memory_order_acq_rel);
        cursor = 0;
        nextCapture = {};
        averageVisited = maximumNativeBatch;
        loggedNativeReady = loggedCameraWaiting = loggedNativeFailure = false;
        log("world actor source ready; worker caches reset by world epoch");
    }
    if (now < nextCapture) return;
    nextCapture = now + std::chrono::milliseconds(25); // Never catch up in a burst.
    NativeReadWindow readWindow;
    auto* local = access.localPlayer();
    const auto localReadyAt = Clock::now();
    if (!local) return;
    const auto cameraPosition = access.actorPosition(local);
    const auto positionReadyAt = Clock::now();
    if (!cameraPosition) {
        if (!loggedCameraWaiting) { loggedCameraWaiting = true;
            log("waiting for native local Actor position component"); }
        return;
    }
    CaptureBatch batch;
    batch.config = config;
    batch.camera = {worldEpoch.load(std::memory_order_acquire), *cameraPosition};
    std::size_t visited = 0;
    if (!access.captureBatch(local, cursor, batch, visited)) {
        if (!loggedNativeFailure) { loggedNativeFailure = true;
            log("incremental Actor capture unavailable [required storage or guarded read failed]"); }
        return;
    }
    const auto actorsReadyAt = Clock::now();
    if (visited) averageVisited = averageVisited * 0.9 + static_cast<double>(visited) * 0.1;
    if (batch.count && !loggedNativeReady) {
        loggedNativeReady = true;
        log("native incremental capture active: pool=", batch.population,
            " [RuntimeID + AABB copies; maximum 8 rows; 250us budget]");
    }
    // Keep live records across a full incremental pass, even if the time budget
    // reduced the number of rows captured on a busy callback.
    batch.liveLifetime = std::chrono::milliseconds(std::clamp<std::size_t>(
        batch.population * 100 / std::max<std::size_t>(1, static_cast<std::size_t>(averageVisited)),
        750, 30000));
    gameHeartbeatMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count(), std::memory_order_release);
    for (auto& worker : processingWorkers) {
        std::unique_lock lock(worker.mutex, std::try_to_lock);
        if (!lock || worker.stopping) continue;
        worker.pending = batch;
        lock.unlock();
        worker.ready.notify_one();
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-now).count();
    if ((config->debug || elapsed >= 2000) && now >= nextReport) {
        nextReport = now + std::chrono::seconds(10);
        const auto us = [](auto from, auto to) {
            return std::chrono::duration_cast<std::chrono::microseconds>(to - from).count();
        };
        log("HitBox native capture us=", elapsed,
            " local=", us(now, localReadyAt), " position=", us(localReadyAt, positionReadyAt),
            " actors=", us(positionReadyAt, actorsReadyAt),
            " publish=", us(actorsReadyAt, Clock::now()), " pool=", batch.population,
            " visited=", visited, " copied=", batch.count,
            " [8 rows maximum; cooperative 250us actor budget; 2 workers]");
    }
}
void observeWorld(const bedrock::LevelTickEvent& event) {
    if (!event.level) return;
    currentWorld.store(event.level, std::memory_order_release);
    try { refreshGameTickSnapshot(); }
    catch (...) { log("native capture skipped; retaining worker snapshots"); }
}

// Win64 prototypes corroborated by exact-build register use and native callers.
// Borrow game-owned render objects only during LevelRenderer::renderLevel.
using RenderLevelFn = void(__fastcall*)(void*, void*, void*);
using BeginFn = void(__fastcall*)(void*, int, int, int, bool);
using VertexFn = void(__fastcall*)(void*, float, float, float);
struct FloatColor { float r,g,b,a; };
using ColorFn = void(__fastcall*)(void*, const FloatColor*);
using MeshFn = void(__fastcall*)(void*, void*, void*, void*);
hitbox::WorldRenderProfile worldRenderProfile;
RenderLevelFn originalRenderLevel{};
BeginFn tessBegin{};
VertexFn tessVertex{};
ColorFn tessColor{};
MeshFn renderMesh{};
BDS_HookToken renderHookToken{};
std::atomic<bool> worldRenderFault{};

// This mod is pinned for process lifetime. On Stop the direct hook remains a
// forwarding trampoline: removing it while another render call uses its original
// would free executable code still in flight. No worker/native data is touched.
constexpr std::size_t maximumRenderedBoxes = 512;
struct NativeDrawRange { const BoxRecord* boxes{}; std::size_t count{};
    std::chrono::milliseconds liveLifetime{}; };
struct NativeDrawInput {
    std::array<NativeDrawRange, workerCount> ranges{};
    Config config;
    Clock::time_point now;
};

template<class T>
bool safeWrite(void* base, std::size_t offset, const T& value) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto start=reinterpret_cast<std::uintptr_t>(base);
    if (!start || start>UINTPTR_MAX-offset) return false;
    auto* dest=reinterpret_cast<T*>(start+offset);
    if (!permittedAddress(dest,sizeof(T),false,true)) return false;
    __try { *dest=value; return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int drawNativeGuarded(void* renderer, void* screen, const NativeDrawInput* input) noexcept {
    // No owning C++ objects inside SEH. Snapshots remain owned by drawWorldBoxes.
    const auto& p=worldRenderProfile;
    void* player=nullptr; void* tess=nullptr; void* shader=nullptr; void* materialInfo=nullptr;
    bedrock::Vec3 origin{}, oldTranslation{};
    FloatColor oldColor{}, white{1,1,1,1};
    std::uint8_t building=0, special=0;
    if (!safeRead(renderer,p.player,player) || !player ||
        !safeRead(player,p.origin,origin) || !std::isfinite(origin.x) ||
        !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
        !safeRead(player,p.material,materialInfo) || !materialInfo ||
        !permittedAddress(materialInfo,sizeof(void*)) ||
        !safeRead(screen,p.tess,tess) || !tess ||
        !safeRead(screen,p.shader,shader) || !shader ||
        !safeRead(tess,p.building,building) || building ||
        !safeRead(tess,p.special,special) || special ||
        !safeRead(tess,p.translation,oldTranslation) || !safeRead(shader,0,oldColor)) return 0;
    const bedrock::Vec3 zero{};
    const std::uint8_t dirty=1;
    if (!safeWrite(tess,p.translation,zero)) return 0;
    if (!safeWrite(shader,0,white) || !safeWrite(shader,p.dirty,dirty)) {
        safeWrite(shader,0,oldColor); safeWrite(shader,p.dirty,dirty);
        safeWrite(tess,p.translation,oldTranslation); return 0;
    }
    bool success=true;
    __try {
        // Mode 4 is the game's LineList. 24 vertices / AABB, one native mesh flush.
        std::size_t total=0;
        for (const auto& range:input->ranges) total+=range.count;
        total=std::min(total,maximumRenderedBoxes);
        tessBegin(tess,0,4,static_cast<int>(total*24),false);
        std::size_t drawn=0;
        for (const auto& range:input->ranges) {
            for (std::size_t i=0;i<range.count && drawn<total;++i,++drawn) {
                const auto& box=range.boxes[i];
                const auto packed=input->config.retainLastPosition &&
                    input->now-box.seenAt>range.liveLifetime
                    ? input->config.retainedColor : input->config.liveColor;
                const FloatColor color{(packed&255)/255.f,((packed>>8)&255)/255.f,
                    ((packed>>16)&255)/255.f,((packed>>24)&255)/255.f};
                tessColor(tess,&color);
                const auto vertices=hitbox::worldLineVertices(box,origin);
                for (const auto& v:vertices) tessVertex(tess,v.x,v.y,v.z);
            }
        }
        alignas(16) std::array<std::byte,0x58> scratch{};
        renderMesh(screen,tess,static_cast<std::byte*>(player)+p.material,scratch.data());
    } __except(EXCEPTION_EXECUTE_HANDLER) { success=false; }
    // Restore the borrowed scalar state, also after a guarded native fault.
    safeWrite(shader,0,oldColor); safeWrite(shader,p.dirty,dirty);
    safeWrite(tess,p.translation,oldTranslation);
    return success ? 1 : -1;
}
void drawWorldBoxes(void* renderer, void* screen) {
    if (stopRequested.load(std::memory_order_acquire) || worldRenderFault.load()) return;
    const auto config=std::atomic_load_explicit(&liveConfig,std::memory_order_acquire);
    if (!config->enabled) return;
    const auto now=Clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()-
        gameHeartbeatMs.load(std::memory_order_acquire)>1000) return;
    const auto epoch=worldEpoch.load(std::memory_order_acquire);
    std::array<std::shared_ptr<const RenderSnapshot>,workerCount> snapshots;
    NativeDrawInput input; input.config=*config; input.now=now;
    std::size_t count=0;
    for (std::size_t i=0;i<workerCount;++i) {
        snapshots[i]=std::atomic_load_explicit(&processingWorkers[i].snapshot,std::memory_order_acquire);
        if (!snapshots[i] || snapshots[i]->epoch!=epoch) continue;
        input.ranges[i]={snapshots[i]->boxes.data(),snapshots[i]->boxes.size(),snapshots[i]->liveLifetime};
        count+=input.ranges[i].count;
    }
    if (!count || worldEpoch.load(std::memory_order_acquire)!=epoch) return;
    NativeReadWindow window;
    const auto result=drawNativeGuarded(renderer,screen,&input);
    static std::atomic<bool> loggedDraw{};
    if (result>0 && !loggedDraw.exchange(true))
        log("native world mesh submitted [12 edges/box; engine render origin + selection_box]");
    if (result<0 && !worldRenderFault.exchange(true))
        log("world rendering disabled after guarded native fault [restart required]");
}
void __fastcall renderLevelDetour(void* renderer, void* screen, void* context) {
    originalRenderLevel(renderer,screen,context);
    // The native world pass supplies view/projection matrices, origin and depth.
    static thread_local bool drawing=false;
    if (drawing) return;
    drawing=true;
    try { drawWorldBoxes(renderer,screen); }
    catch (...) { if(!worldRenderFault.exchange(true)) log("world rendering disabled after exception"); }
    drawing=false;
}
bool installWorldRenderer(std::string& error) {
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::array<void*,5> addresses{};
    for (std::size_t i=0;i<addresses.size();++i) {
        const auto& f=worldRenderProfile.functions[i];
        auto* target=reinterpret_cast<const std::byte*>(base+f.rva);
        if (!isExecutable(target)) { error="world render target is not executable"; return false; }
        for (std::size_t j=0;j<f.guard.size();++j) {
            std::uint8_t value{};
            if(!safeRead(target,j,value) || value!=f.guard[j]) {
                error="world render entry guard mismatch: function "+std::to_string(i); return false;
            }
        }
        addresses[i]=const_cast<std::byte*>(target);
    }
    tessBegin=reinterpret_cast<BeginFn>(addresses[1]); tessVertex=reinterpret_cast<VertexFn>(addresses[2]);
    tessColor=reinterpret_cast<ColorFn>(addresses[3]); renderMesh=reinterpret_cast<MeshFn>(addresses[4]);
    return BDS_HookDirectInstall(addresses[0],reinterpret_cast<void*>(&renderLevelDetour),
        reinterpret_cast<void**>(&originalRenderLevel),&renderHookToken)!=0 ||
        (error="world render direct hook conflict/install failure",false);
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
    try { startLogWorker(directory / L"HitBox.log"); }
    catch (...) { status.store(Status::Failed); return 1; }
    struct LogStop { ~LogStop() { stopLogWorker(); } } logStop;
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
    const auto identity = choice.identity;
    const auto prepareProfile = [&](std::string_view candidate) {
        // Profile selection checks executable identity, but an older cache can
        // still lack layouts required by a newer mod. Validate the whole set
        // before accepting it or installing any native hooks.
        bedrock::Resolver candidateResolver;
        bedrock::NativeItemLayout candidateItems;
        bedrock::NativeActorLayout candidateActors;
        if (!candidateResolver.loadJson(candidate, error) ||
            !candidateResolver.attachWindowsMainModule(identity, error) ||
            !candidateResolver.exactBuild()) return false;
        if (!bedrock::parseNativeItemLayout(candidate, candidateItems, error)) {
            error = "native_items: " + error;
            return false;
        }
        if (!bedrock::parseNativeActorLayout(candidate, candidateActors, error)) {
            error = "native_actors: " + error;
            return false;
        }
        hitbox::WorldRenderProfile candidateRender;
        if (!hitbox::parseWorldRenderProfile(candidate, candidateRender, error)) return false;
        worldRenderProfile = std::move(candidateRender);
        resolver = std::move(candidateResolver);
        itemLayout = std::move(candidateItems);
        actorLayout = std::move(candidateActors);
        return true;
    };
    bool profileReady = prepareProfile(profile);
    if (!profileReady) {
        log("profile lacks required native layouts; checking GitHub: ", error);
        std::string updatedProfile;
        bedrock::ProfileChoice updatedChoice;
        if (bedrock::refreshSupportedProfile(identity, updatedProfile, updatedChoice, error,
                [](std::string_view stage) { log("profile ", stage); }) &&
            prepareProfile(updatedProfile)) {
            profile = std::move(updatedProfile);
            choice = std::move(updatedChoice);
            profileReady = true;
            log("native layouts recovered from latest exact-build profile");
        } else {
            log("latest native profile unavailable: ", error);
        }
    }
    if (!profileReady && prepareProfile(bedrock::generated::kSymbolDatabase)) {
        // Reuse the original live fingerprint. A bundled profile from another
        // executable/build is rejected by attachWindowsMainModule above.
        choice.source = bedrock::ProfileSource::Embedded;
        profileReady = true;
        log("native layouts recovered from embedded exact-build profile");
    }
    if (!profileReady) {
        log("exact-build native Actor profile unavailable: ", error);
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    access.setLayouts(std::move(itemLayout), std::move(actorLayout));
    log("native Actor registry source ready [profile candidate; fail-closed reads]");

    if (!startProcessingWorkers()) { status.store(Status::Failed); return 1; }
    struct WorkerStop { ~WorkerStop() { stopProcessingWorkers(); } } workerStop;
    log("2 processing workers + 1 log writer ready [copied scalars only]");
    gameHooks = std::make_unique<bedrock::GameHooks>(gameApi);
    gameApi.Event.Enum.Gameplay.then([](const bedrock::GameplayEvent& event) {
        if (event.clientInstance) access.setClient(event.clientInstance);
    });
    gameApi.Event.Enum.LevelTick.then(&observeWorld);
    if (!gameHooks->installGameplay(resolver, error) ||
        !gameHooks->installLevelTick(resolver, error)) {
        log("SDK hook registration failed: ", error);
        gameHooks.reset();
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    if (!installWorldRenderer(error)) {
        log("world renderer install failed: ", error);
        gameHooks.reset();
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    log("native world rendering active [LevelRenderer::renderLevel + selection_box; exact-build guards; runtime candidate]");
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
