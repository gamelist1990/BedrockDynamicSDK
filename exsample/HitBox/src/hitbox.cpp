#include <bedrock/profile_json.hpp>
#ifdef _WIN32
#if !defined(_MSC_VER)
#error "HitBox native EnTT access requires the Microsoft C++ ABI (MSVC or clang-cl)."
#endif
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
#if _ITERATOR_DEBUG_LEVEL != 0
#error "HitBox must use the release STL layout: build Release with iterator debugging disabled."
#endif

#include <nlohmann/json.hpp>
#include "hitbox/box_cache.hpp"
#include "hitbox/latest_pose.hpp"
#include "hitbox/player_name_archive.hpp"
#include "hitbox/batch_queue.hpp"
#include "hitbox/world_render_profile.hpp"
#include "hitbox/name_tags.hpp"
#include "hitbox/prepared_names.hpp"
#include "hitbox/name_mesh_win32.hpp"
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
    bool showAllActors = false;
    bool playersOnly = false;
    bool showNameTags = true;
    float nameTagScale = 1.0f;
    bool retainLastPosition = false;
    float nearbyDistance = 100.0f;
    int retentionMinutes = 10;
    int nameCacheMinutes = 10;
    float lineThickness = 1.5f;
    std::uint32_t liveColor = packColor(76, 230, 132, 255);
    std::uint32_t retainedColor = packColor(255, 174, 66, 255);
};

using BoxRecord = hitbox::BoxRecord;
hitbox::LatestPoseMailbox latestPoses;
hitbox::PlayerNameArchive playerNames;
struct CameraState {
    std::uint64_t epoch{};
    bedrock::Vec3 cameraPosition{};
};
struct RenderSnapshot {
    std::uint64_t epoch{};
    std::vector<BoxRecord> boxes;
    std::vector<std::shared_ptr<const hitbox::NameMesh>> nameMeshes;
    std::shared_ptr<const hitbox::PreparedNames> preparedNames;
    std::chrono::milliseconds liveLifetime{1000};
};
constexpr std::size_t workerCapacity = 3;
std::size_t workerCount = 1; // Selected once before workers/hooks start; never changes live.
constexpr std::size_t maximumNativeBatch = hitbox::CaptureSchedule::maximumBatch;
struct CaptureBatch {
    CameraState camera;
    std::array<hitbox::Sample, maximumNativeBatch> samples;
    std::size_t count{};
    std::size_t population{};
    std::size_t tickPoseCount{};
    std::uint64_t tickPoseUs{};
    const char* captureStage="registry_context";
    const char* captureFailure=nullptr;
    std::uint32_t faultCode{};
    std::uintptr_t faultAddress{};
    // Per-batch observations; aggregate on the game thread for periodic diagnostics.
    std::size_t nativePlayers{}, rememberedPlayers{}, missingRuntime{}, filteredActors{},
                invalidOwner{}, missingShape{}, invalidBounds{};
    std::chrono::milliseconds liveLifetime{1000};
    std::shared_ptr<const Config> config;
};
struct ProcessingWorker {
    std::mutex mutex;
    std::condition_variable ready;
    hitbox::BatchQueue<CaptureBatch,8> pending; // Bounded FIFO; captured Actors are never overwritten.
    std::thread thread;
    bool stopping{};
    std::shared_ptr<const RenderSnapshot> snapshot;
};
std::array<ProcessingWorker, workerCapacity> processingWorkers;
std::size_t actorWorker(std::uint64_t id) noexcept {
    id=(id^(id>>30))*0xbf58476d1ce4e5b9ULL;
    id=(id^(id>>27))*0x94d049bb133111ebULL;
    return static_cast<std::size_t>(id^(id>>31))%workerCount;
}
const auto trackingBudget=std::make_shared<hitbox::TrackingBudget>();
std::atomic<std::uint64_t> worldEpoch{};
std::atomic<std::uintptr_t> localActorIdentity{};
std::atomic<std::int64_t> gameHeartbeatMs{};
// Only copied scalars cross threads. The render callback never waits on workers.
struct RenderCameraMailbox {
    std::mutex mutex;
    std::uint64_t epoch{};
    bedrock::Vec3 origin{};
    void publish(std::uint64_t value,const bedrock::Vec3& position) {
        std::unique_lock lock(mutex,std::try_to_lock);
        if(lock) { epoch=value; origin=position; }
    }
    std::optional<bedrock::Vec3> read(std::uint64_t value) {
        std::lock_guard lock(mutex);
        return epoch==value ? std::optional<bedrock::Vec3>{origin} : std::nullopt;
    }
} renderCamera;
std::atomic<std::uint64_t> renderBoundaryChanges{};

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
        {"show_all_actors", config.showAllActors},
        {"players_only", config.playersOnly},
        {"show_name_tags", config.showNameTags},
        {"name_tag_scale", config.nameTagScale},
        {"nearby_distance", config.nearbyDistance},
        {"retain_last_position", config.retainLastPosition},
        {"retention_minutes", config.retentionMinutes},
        {"name_cache_minutes", config.nameCacheMinutes},
        {"line_thickness", config.lineThickness},
        {"live_color", colorString(config.liveColor)},
        {"retained_color", colorString(config.retainedColor)}};
}

Config parseConfig(const nlohmann::json& root) {
    Config config;
    config.enabled = root.value("enabled", config.enabled);
    config.debug = root.value("debug", config.debug);
    config.showAllActors = root.value("show_all_actors", config.showAllActors);
    config.playersOnly = root.value("players_only", config.playersOnly);
    config.showNameTags = root.value("show_name_tags", config.showNameTags);
    config.nameTagScale = root.value("name_tag_scale", config.nameTagScale);
    config.nameCacheMinutes=std::clamp(root.value("name_cache_minutes",config.nameCacheMinutes),1,120);
    if(!std::isfinite(config.nameTagScale)) config.nameTagScale=1.0f;
    config.nameTagScale=std::clamp(config.nameTagScale,0.25f,3.0f);
    config.nearbyDistance = std::clamp(root.value("nearby_distance", config.nearbyDistance), 4.0f, 128.0f);
    config.retainLastPosition = root.value("retain_last_position", config.retainLastPosition);
    // Retained observations always expire after ten minutes, including legacy configs.
    config.retentionMinutes = 10;
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
    void setLayouts(bedrock::NativeItemLayout items, bedrock::NativeActorLayout actors,
                    std::size_t actorTypeOffset, std::uint32_t playerType) {
        itemLayout_ = std::move(items);
        actorLayout_ = std::move(actors);
        actorTypeValueOffset_=actorTypeOffset; playerTypeId_=playerType;
    }
    void setClient(void* client) noexcept { client_.store(client, std::memory_order_release); }
    void* clientIdentity() const noexcept { return client_.load(std::memory_order_acquire); }
    void setWorld(void* world) noexcept { world_.store(world, std::memory_order_release); }
    void resetPlayerTracking() noexcept { knownPlayers_.reset(); }
    void setPlayerListOffset(std::size_t offset) noexcept {playerListOffset_=offset;}
    bool copyNameEntityId(void* actor,std::uint32_t& id) const noexcept {
        return safeRead(actor,actorLayout_.actorEntityContextOffset+actorLayout_.contextEntityIdOffset,id);
    }

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
            cached = {this, client, world, actor, registry, entity, Clock::now()+std::chrono::milliseconds(50)};
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

    std::uint64_t runtimeIdentity(void* actor) const noexcept {
        __try {
            void* registry=nullptr;
            std::uint32_t entity=0;
            const auto context=actorLayout_.actorEntityContextOffset;
            if(!safeRead(actor,context+actorLayout_.contextRegistryOffset,registry) || !registry ||
                !safeRead(actor,context+actorLayout_.contextEntityIdOffset,entity)) return 0;
            using Registry=entt::basic_registry<hitbox_native::EntityId>;
            auto* typed=static_cast<Registry*>(registry);
            const hitbox_native::EntityId handle{entity};
            auto* storage=typed->storage(actorLayout_.runtimeIdComponentHash);
            std::uint64_t runtime=0;
            if(!typed->valid(handle) || !storage || !storage->contains(handle) ||
                !safeRead(storage->value(handle),actorLayout_.runtimeIdValueOffset,runtime)) return 0;
            return runtime;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }
    std::uintptr_t registryIdentity(void* actor) const noexcept {
        void* registry=nullptr;
        return safeRead(actor,actorLayout_.actorEntityContextOffset+actorLayout_.contextRegistryOffset,registry)
            ? reinterpret_cast<std::uintptr_t>(registry) : 0;
    }
    bool captureBatch(void* local, std::size_t& cursor, hitbox::CaptureSchedule& schedule, CaptureBatch& batch,
                      std::size_t& visited) const noexcept {
        // No EnTT iterator/storage/component pointer survives the callback.
        visited = 0;
        void* registry = nullptr;
        const auto& layout = actorLayout_;
        if (!safeRead(local, layout.actorEntityContextOffset + layout.contextRegistryOffset,
                      registry) || !registry) {
            batch.captureFailure="local_registry_unreadable";
            return false;
        }
        return captureRegistryBatch(registry, local, layout, actorTypeValueOffset_, playerTypeId_, &knownPlayers_, &cursor, &schedule, &batch, &visited);
    }
    void capturePlayerNames(void* world,std::uint64_t epoch,int minutes) const noexcept {
        if(!playerListOffset_)return;
        const auto now=Clock::now();
        if(now<nextRoster_ && rosterEpoch_==epoch)return;
        nextRoster_=now+std::chrono::seconds(1);rosterEpoch_=epoch;
        const auto count=copyPlayerNames(world,playerListOffset_,rosterCopies_.data());
        if(count!=SIZE_MAX)playerNames.publish(epoch,rosterCopies_.data(),count,std::chrono::minutes(minutes));
    }
    // Keep stable EnTT handles for already discovered Actors. New Actor discovery
    // stays bounded; positions of all tracked live handles are copied once per
    // LevelTick, without walking Actor objects, names or equipment again.
    void refreshTrackedPoses(void* local,CaptureBatch& batch) const noexcept {
        const auto begin=Clock::now();
        if(poseEpoch_!=batch.camera.epoch) {poseEpoch_=batch.camera.epoch;poseCount_=0;}
        for(std::size_t n=0;n<batch.count;++n) {
            const auto& sample=batch.samples[n];
            std::size_t i=0;
            for(;i<poseCount_;++i) if(poseHandles_[i].runtimeId==sample.runtimeId)break;
            if(i==poseCount_) {if(poseCount_==poseHandles_.size())continue;++poseCount_;}
            poseHandles_[i]=sample;
        }
        void* registry=nullptr;
        if(!safeRead(local,actorLayout_.actorEntityContextOffset+actorLayout_.contextRegistryOffset,registry)||!registry)return;
        batch.tickPoseCount=copyTrackedPoses(registry,actorLayout_,poseHandles_.data(),&poseCount_,poseCopies_.data());
        latestPoses.publish(batch.camera.epoch,poseCopies_.data(),batch.tickPoseCount);
        batch.tickPoseUs=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-begin).count());
    }
private:
    struct NativePlayerEntry {std::uint64_t uniqueId;hitbox::PlayerUuid uuid;std::string name;std::byte rest[0xA8-0x18-sizeof(std::string)];};
    static_assert(sizeof(NativePlayerEntry)==0xA8);
    using NativePlayerList=std::unordered_map<hitbox::PlayerUuid,NativePlayerEntry,hitbox::PlayerUuidHash>;
    static std::size_t copyPlayerNames(void* world,std::size_t offset,hitbox::CopiedPlayerName* copies) noexcept {
        std::size_t count=0;
        __try {
            NativePlayerList* list=nullptr;
            if(!safeRead(world,offset,list)||!list||list->size()>512)return SIZE_MAX;
            for(const auto& item:*list){auto& target=copies[count];target={};
                target.uuid=item.first;target.uniqueId=item.second.uniqueId;
                const auto& text=item.second.name;
                if(text.size()<=4096){target.size=std::min(text.size(),target.bytes.size());
                    if(target.size)std::memcpy(target.bytes.data(),text.data(),target.size);}
                if(++count==512)break;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {return SIZE_MAX;}
        return count;
    }
    std::size_t playerListOffset_{};
    mutable Clock::time_point nextRoster_{};mutable std::uint64_t rosterEpoch_{};
    mutable std::array<hitbox::CopiedPlayerName,512> rosterCopies_{};
    static std::size_t copyTrackedPoses(void* registry,const bedrock::NativeActorLayout& layout,
        hitbox::Sample* handles,std::size_t* count,hitbox::Sample* copies) noexcept {
        std::size_t copied=0;
        __try {
            using Registry=entt::basic_registry<hitbox_native::EntityId>;
            auto* typed=static_cast<Registry*>(registry);
            auto* runtime=typed->storage(layout.runtimeIdComponentHash);
            auto* shape=typed->storage(layout.aabbShapeComponentHash);
            if(!runtime || !shape)return 0;
            const auto now=Clock::now();
            for(std::size_t i=0;i<*count;) {
                auto sample=handles[i];const hitbox_native::EntityId entity{sample.entityId};
                std::uint64_t id=0;
                if(!typed->valid(entity) || !runtime->contains(entity) ||
                   !safeRead(runtime->value(entity),layout.runtimeIdValueOffset,id) || id!=sample.runtimeId) {
                    handles[i]=handles[--*count];continue;
                }
                ++i;
                if(!shape->contains(entity))continue;
                auto* value=shape->value(entity);
                if(layout.aabbMaximumOffset==layout.aabbMinimumOffset+sizeof(bedrock::Vec3)) {
                    if(!safeRead(value,layout.aabbMinimumOffset,sample.bounds))continue;
                } else if(!safeRead(value,layout.aabbMinimumOffset,sample.bounds.minimum)||
                          !safeRead(value,layout.aabbMaximumOffset,sample.bounds.maximum))continue;
                const auto& b=sample.bounds;
                if(!b.valid() || b.maximum.x<=b.minimum.x || b.maximum.y<=b.minimum.y || b.maximum.z<=b.minimum.z)continue;
                sample.seenAt=now;handles[i-1]=sample;copies[copied++]=sample;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        return copied;
    }
    mutable std::uint64_t poseEpoch_{};
    mutable std::size_t poseCount_{};
    mutable std::array<hitbox::Sample,hitbox::TrackingBudget::maximumActors> poseHandles_{},poseCopies_{};
    static bool captureRegistryBatch(void* registry, void* local, const bedrock::NativeActorLayout& layout,
        std::size_t typeOffset, std::uint32_t playerType, hitbox::PlayerIdentityCache* identities, std::size_t* cursor, hitbox::CaptureSchedule* schedule, CaptureBatch* batch, std::size_t* visited) noexcept {
        __try {
            using Registry = entt::basic_registry<hitbox_native::EntityId>;
            auto* typed = static_cast<Registry*>(registry);
            batch->captureStage="lookup_actor_owner_storage";
            auto* owner = typed->storage(layout.actorOwnerComponentHash);
            if (!owner) { batch->captureFailure="actor_owner_storage_missing"; return false; }
            batch->captureStage="lookup_runtime_id_storage";
            auto* runtime = typed->storage(layout.runtimeIdComponentHash);
            auto* unique = typed->storage(layout.actorUniqueIdComponentHash);
            if (!runtime) { batch->captureFailure="runtime_id_storage_missing"; return false; }
            batch->captureStage="lookup_aabb_storage";
            auto* shape = typed->storage(layout.aabbShapeComponentHash);
            batch->captureStage="lookup_actor_type_storage";
            auto* actorType = typed->storage(layout.actorTypeComponentHash);
            auto* candidates = owner;
            // A fixed source keeps the incremental cursor stable when pool sizes change.
            batch->captureStage="owner_pool_size";
            batch->population = candidates->size();
            if (batch->population > 16384) {
                batch->captureFailure="owner_pool_size_out_of_range"; return false;
            }
            if (!batch->population) { *cursor = 0; schedule->beginTick(0); return true; }
            const auto begin = Clock::now();
            const auto count = schedule->beginTick(batch->population);
            for (std::size_t n = 0; n < count; ++n) {
                // Cooperative 250us budget; a single native read cannot be preempted.
                if (n && Clock::now() - begin >= std::chrono::microseconds(250)) break;
                if (*cursor >= batch->population) *cursor = 0;
                batch->captureStage="owner_pool_entry";
                const auto entity = candidates->data()[(*cursor)++];
                ++*visited;
                schedule->visited(1);
                batch->captureStage="validate_registry_entity";
                if (!typed->valid(entity) || !owner->contains(entity)) continue;
                batch->captureStage="read_runtime_id_component";
                std::uint64_t runtimeId=0;
                if (!runtime->contains(entity) ||
                    !safeRead(runtime->value(entity), layout.runtimeIdValueOffset, runtimeId) || !runtimeId) {
                    ++batch->missingRuntime; continue;
                }
                batch->captureStage="read_actor_type_component";
                std::uint32_t type=0;
                const bool nativePlayer=actorType && actorType->contains(entity) &&
                    safeRead(actorType->value(entity),typeOffset,type) && type==playerType;
                const bool isPlayer=nativePlayer || identities->known(runtimeId);
                if(batch->config->playersOnly && !isPlayer) { ++batch->filteredActors; continue; }
                // Validate the owner against the exact registry/entity once, rather
                // than validating the Actor vtable for every property wrapper.
                batch->captureStage="read_actor_owner_component";
                void* actor = nullptr;
                if (!safeRead(owner->value(entity), layout.actorOwnerPointerOffset, actor) || !actor) {
                    ++batch->invalidOwner; continue;
                }
                // Exclude self before AABB/name reads, tracking and retention.
                if(actor==local) continue;
                batch->captureStage="validate_actor_context";
                void* actorRegistry = nullptr;
                std::uint32_t actorEntity = 0;
                const auto context = layout.actorEntityContextOffset;
                if (!safeRead(actor, context + layout.contextRegistryOffset, actorRegistry) ||
                    actorRegistry != registry ||
                    !safeRead(actor, context + layout.contextEntityIdOffset, actorEntity) ||
                    actorEntity != static_cast<std::uint32_t>(entity)) {
                    ++batch->invalidOwner; continue;
                }
                if (nativePlayer) {
                    identities->classify(runtimeId,true);
                    ++batch->nativePlayers;
                }
                else if (isPlayer) ++batch->rememberedPlayers;
                batch->captureStage="read_aabb_component";
                if (!shape || !shape->contains(entity)) { ++batch->missingShape; continue; }
                auto& sample = batch->samples[batch->count];
                sample.isPlayer=isPlayer;
                sample.runtimeId=runtimeId;
                sample.actorKey=reinterpret_cast<std::uintptr_t>(actor);
                sample.entityId=static_cast<std::uint32_t>(entity);
                sample.uniqueId=0;
                if(unique && unique->contains(entity))
                    safeRead(unique->value(entity),layout.actorUniqueIdValueOffset,sample.uniqueId);
                auto* aabb = shape->value(entity);
                if (layout.aabbMaximumOffset == layout.aabbMinimumOffset + sizeof(bedrock::Vec3)) {
                    if (!safeRead(aabb, layout.aabbMinimumOffset, sample.bounds)) {
                        ++batch->invalidBounds; continue;
                    }
                } else if (!safeRead(aabb, layout.aabbMinimumOffset, sample.bounds.minimum) ||
                           !safeRead(aabb, layout.aabbMaximumOffset, sample.bounds.maximum)) {
                    ++batch->invalidBounds; continue;
                }
                if (!sample.bounds.valid()) { ++batch->invalidBounds; continue; }
                const auto& b = sample.bounds;
                if (b.maximum.x - b.minimum.x <= 0 ||
                    b.maximum.y - b.minimum.y <= 0 ||
                    b.maximum.z - b.minimum.z <= 0) { ++batch->invalidBounds; continue; }
                sample.seenAt = Clock::now();
                ++batch->count;
            }
            batch->captureStage="complete";
            return true;
        } __except(captureFault(GetExceptionInformation(),batch)) {
            return batch->count != 0; // Publish any rows copied before the failed read.
        }
    }
    static int captureFault(EXCEPTION_POINTERS* exception,CaptureBatch* batch) noexcept {
        batch->captureFailure="guarded_native_exception";
        if (exception && exception->ExceptionRecord) {
            batch->faultCode=exception->ExceptionRecord->ExceptionCode;
            batch->faultAddress=reinterpret_cast<std::uintptr_t>(exception->ExceptionRecord->ExceptionAddress);
        }
        return EXCEPTION_EXECUTE_HANDLER;
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
    std::size_t actorTypeValueOffset_{};
    std::uint32_t playerTypeId_{};
    mutable hitbox::PlayerIdentityCache knownPlayers_;
    std::atomic<void*> client_{};
    std::atomic<void*> world_{};
};

HitBoxAccess access;
bedrock::GameApi gameApi{access};
std::unique_ptr<bedrock::GameHooks> gameHooks;

hitbox::NameMailbox nameMailbox;
BDS_HookToken nameTagToken{};
std::atomic<void*> nameGetter{};
struct NameDiagnostics {
    std::atomic<std::uint64_t> hookCalls{},hookCopied{},hookRejected{},nativeCalls{},nativeCopied{},
        nativeEmpty{},nativeUnreadable{},nativeFaults{},mailboxMisses{},blankNames{},meshFailures{},workerExceptions{},
        renderCalls{},renderNoMesh{},renderHidden{},renderBudget{},renderSubmitted{},renderFaults{},
        frameNoBoxes{},frameStale{},renderFrames{},renderUs{},renderPeakUs{};
    struct Worker { std::atomic<std::size_t> boxes{},observed{},meshes{},vertices{},prepareUs{},retainedBoxes{},retainedMeshes{},geometryReused{}; };
    std::array<Worker,workerCapacity> workers;
} nameDiagnostics;
void reportNameDiagnostics() {
    static auto next=Clock::time_point{};
    const auto now=Clock::now();
    if(now<next) return;
    next=now+std::chrono::seconds(5);
    const auto config=std::atomic_load_explicit(&liveConfig,std::memory_order_acquire);
    auto& d=nameDiagnostics;
    log("NameTag diagnostic [5s]: epoch=",worldEpoch.load()," enabled=",config->enabled,
        " show_name_tags=",config->showNameTags," scale=",config->nameTagScale,
        " draw_mode=text_only local_label=hidden getter=",nameGetter.load()," hook_calls=",d.hookCalls.exchange(0),
        " hook_copied=",d.hookCopied.exchange(0)," hook_rejected=",d.hookRejected.exchange(0));
    log("NameTag getter [5s]: calls=",d.nativeCalls.exchange(0)," copied=",d.nativeCopied.exchange(0),
        " empty=",d.nativeEmpty.exchange(0)," unreadable=",d.nativeUnreadable.exchange(0),
        " faults=",d.nativeFaults.exchange(0));
    for(std::size_t i=0;i<workerCount;++i)
        log("NameTag worker state: shard=",i," boxes=",d.workers[i].boxes.load(),
            " matched_names=",d.workers[i].observed.load()," ready_meshes=",d.workers[i].meshes.load(),
            " prepared_vertices=",d.workers[i].vertices.load()," prepare_us=",d.workers[i].prepareUs.load(),
            " retained_boxes=",d.workers[i].retainedBoxes.load()," retained_meshes=",d.workers[i].retainedMeshes.load()," geometry_reused=",d.workers[i].geometryReused.load());
    log("NameTag processing [5s]: mailbox_misses=",d.mailboxMisses.exchange(0),
        " blank_after_normalize=",d.blankNames.exchange(0)," mesh_failures=",d.meshFailures.exchange(0),
        " worker_exceptions=",d.workerExceptions.exchange(0));
    const auto renderFrames=d.renderFrames.exchange(0);
    const auto renderUs=d.renderUs.exchange(0);
    log("HitBox render cost [5s]: frames=",renderFrames," average_us=",renderFrames ? renderUs/renderFrames : 0,
        " peak_us=",d.renderPeakUs.exchange(0)," [native submission only; worker geometry excluded]");
    log("NameTag rendering [5s]: passes=",d.renderCalls.exchange(0)," submitted_labels=",d.renderSubmitted.exchange(0),
        " no_mesh=",d.renderNoMesh.exchange(0)," hidden_or_expired=",d.renderHidden.exchange(0),
        " vertex_budget_skips=",d.renderBudget.exchange(0)," shader_write_failures=",d.renderFaults.exchange(0),
        " frames_no_boxes=",d.frameNoBoxes.exchange(0)," frames_stale_tick=",d.frameStale.exchange(0));
}
bool copyObservedName(const void* native,char* bytes,std::size_t* count) noexcept {
    __try {
        const auto* name=static_cast<const std::string*>(native);
        if(!name || name->empty() || name->size()>4096) return false;
        *count=std::min(name->size(),hitbox::NameMailbox::maximumBytes);
        const auto* source=name->data();
        if(!permittedAddress(source,*count)) return false;
        std::memcpy(bytes,source,*count);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void BDS_HOOK_CALL observeNameTag(void*,void* raw,BDS_HookNext next,void* frame) noexcept {
    next(frame,raw);
    ++nameDiagnostics.hookCalls;
    if(stopRequested.load(std::memory_order_acquire)) return;
    try {
        const auto config=std::atomic_load_explicit(&liveConfig,std::memory_order_acquire);
        if(!config->enabled || !config->showNameTags) return;
        const auto epoch=worldEpoch.load(std::memory_order_acquire);
        if(!epoch || !raw) return;
        const auto& args=*static_cast<BDS_ActorNameTagArgs*>(raw);
        NativeReadWindow window;
        std::uint32_t entity=0;
        std::array<char,hitbox::NameMailbox::maximumBytes> bytes{};
        std::size_t count=0;
        const auto runtimeId=args.actor ? access.runtimeIdentity(args.actor) : 0;
        if(!args.actor || !runtimeId || !access.copyNameEntityId(args.actor,entity) ||
            !copyObservedName(args.name_tag,bytes.data(),&count) ||
            epoch!=worldEpoch.load(std::memory_order_acquire)) { ++nameDiagnostics.hookRejected; return; }
        ++nameDiagnostics.hookCopied;
        nameMailbox.observe({epoch,runtimeId,reinterpret_cast<std::uintptr_t>(args.actor),entity},
            std::string_view(bytes.data(),count));
    } catch(...) {}
}
// Only called on the game thread for a freshly validated capture sample.
// Missing names are spread over ticks; known names use the observation cache.
int copyMissingName(void* target,void* actor,char* bytes,std::size_t* count) noexcept {
    using Getter=const std::string&(__fastcall*)(void*);
    __try {
        const auto& name=reinterpret_cast<Getter>(target)(actor);
        if(name.empty()) return 0;
        return copyObservedName(&name,bytes,count) ? 1 : -2;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
void fillMissingName(const CaptureBatch& batch) {
    if(!batch.config->showNameTags || !batch.count) return;
    auto* target=nameGetter.load(std::memory_order_acquire);
    if(!target) return;
    struct Attempt { hitbox::NameKey key; Clock::time_point next; unsigned failures{}; };
    static std::array<Attempt,2048> attempts{};
    static Clock::time_point nextBatch{};
    static std::size_t nextSample{};
    const auto now=Clock::now();
    if(now<nextBatch) return;
    const auto start=nextSample%batch.count;
    std::size_t calls=0;
    // First serve missing Player names, then other captured Actors. This scans
    // only the existing bounded capture batch, never the world a second time.
    for(unsigned pass=0;pass<2;++pass) for(std::size_t n=0;n<batch.count;++n) {
        if(calls>=3 || (calls && Clock::now()-now>=std::chrono::microseconds(200))) return;
        const auto i=(start+n)%batch.count;
        const auto& sample=batch.samples[i];
        if(sample.isPlayer!=(pass==0)) continue;
        const hitbox::NameKey key{batch.camera.epoch,sample.runtimeId,sample.actorKey,sample.entityId};
        if(nameMailbox.cachedOrBusy(key,std::chrono::minutes(batch.config->nameCacheMinutes))) continue;
        auto& attempt=attempts[(sample.runtimeId^(sample.runtimeId>>32))%attempts.size()];
        if(attempt.key==key && now<attempt.next) continue;
        if(attempt.key!=key) attempt={key,{},0};
        attempt.next=now+std::chrono::milliseconds(100);
        nextBatch=now+std::chrono::milliseconds(20);
        nextSample=(i+1)%batch.count;
        ++calls;
        std::array<char,hitbox::NameMailbox::maximumBytes> bytes{};
        std::size_t count=0;
        ++nameDiagnostics.nativeCalls;
        const int result=copyMissingName(target,reinterpret_cast<void*>(sample.actorKey),bytes.data(),&count);
        if(result==-1) {
            ++nameDiagnostics.nativeFaults;
            nameGetter.store(nullptr,std::memory_order_release);
            log("NameTag fallback fault; native name polling disabled, observation cache remains active");
        } else if(result>0 && batch.camera.epoch==worldEpoch.load(std::memory_order_acquire)) {
            ++nameDiagnostics.nativeCopied;
            nameMailbox.observe(key,std::string_view(bytes.data(),count));
        }
        if(result<=0) {
            attempt.failures=std::min<unsigned>(attempt.failures+1,16);
            attempt.next=now+std::chrono::milliseconds(std::min<unsigned>(2000,100u<<std::min<unsigned>(attempt.failures-1,5)));
        }
        if(result==0) ++nameDiagnostics.nativeEmpty;
        if(result==-2) ++nameDiagnostics.nativeUnreadable;
        static Clock::time_point nextResultLog{};
        if(now>=nextResultLog) {
            nextResultLog=now+std::chrono::seconds(5);
            log("NameTag direct read: epoch=",key.epoch," runtime_id=",sample.runtimeId,
                " actor=",reinterpret_cast<void*>(sample.actorKey)," entity=",sample.entityId,
                " result=",result," bytes=",count," [1=copied 0=empty -1=native fault -2=unreadable]");
        }
        if(result==-1) return;
    }
}

struct CachedName {
    hitbox::NameKey key;
    std::uint64_t revision{},touched{};
    std::shared_ptr<const hitbox::NameMesh> mesh;
    std::string text; // Keep the acquired name even if geometry is deferred/fails.
    Clock::time_point retryAfter{};
    Clock::time_point expires{};
    bool archived{};
};

void processingLoop(std::size_t shard) noexcept {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    auto& worker = processingWorkers[shard];
    hitbox::BoxCache cache{trackingBudget};
    hitbox::NameMesher nameMesher;
    std::unordered_map<std::uint64_t,CachedName> names;
    std::uint64_t nameTouch=0;
    bool fontFailureLogged=false;
    bool nameReadyLogged=false;
    bool publishedEmpty=false;
    std::uint64_t loggedEpoch = 0;
    Clock::time_point nextLog{};
    std::optional<CaptureBatch> lastCapture;
    std::shared_ptr<const RenderSnapshot> previousPrepared;
    std::uint64_t mailboxRevision=0;
    Clock::time_point nextNameExample{},nextPublication{};
    // Retain replaced snapshots until no callback holds them. Their vectors
    // are then destroyed here, instead of on the game's/renderer's last release.
    std::vector<std::shared_ptr<const RenderSnapshot>> retired;
    for (;;) {
        std::optional<CaptureBatch> batch;
        std::array<std::optional<CaptureBatch>,8> incoming;
        std::size_t incomingCount=0;
        {
            std::unique_lock lock(worker.mutex);
            // Consume every observation, but publish geometry at most 20 Hz.
            // Notifications cannot turn this into an unbounded rebuild loop.
            worker.ready.wait_until(lock,nextPublication,[&] { return worker.stopping; });
            worker.ready.wait_for(lock, std::chrono::milliseconds(250), [&] {
                return worker.stopping || !worker.pending.empty();
            });
            if (worker.stopping) return;
            while(incomingCount<incoming.size()) {
                auto value=worker.pending.pop();
                if(!value) break;
                incoming[incomingCount++]=std::move(value);
            }
        }
        try {
            std::erase_if(retired, [](const auto& value) { return value.use_count() == 1; });
            const auto epoch=worldEpoch.load(std::memory_order_acquire);
            playerNames.update();
            if (!cache.beginWorld(epoch)) continue;
            if (loggedEpoch != epoch) {
                loggedEpoch=epoch; nextLog={}; names.clear(); nameTouch=0; nameReadyLogged=false; publishedEmpty=false;
                lastCapture.reset(); previousPrepared.reset(); mailboxRevision=0; nextNameExample={};
                nameDiagnostics.workers[shard].boxes=0;
                nameDiagnostics.workers[shard].observed=0;
                nameDiagnostics.workers[shard].meshes=0;
                nameDiagnostics.workers[shard].vertices=0; nameDiagnostics.workers[shard].prepareUs=0;
                nameDiagnostics.workers[shard].retainedBoxes=0; nameDiagnostics.workers[shard].retainedMeshes=0;
                nameDiagnostics.workers[shard].geometryReused=0;
                const auto previous=std::atomic_exchange_explicit(&worker.snapshot,
                    std::shared_ptr<const RenderSnapshot>{},std::memory_order_acq_rel);
                if(previous) retired.push_back(previous);
            }
            for(std::size_t n=0;n<incomingCount;++n) {
                const auto& captured=*incoming[n];
                if(captured.camera.epoch!=epoch) continue;
                for(std::size_t i=0;i<captured.count;++i) {
                    const auto& sample=captured.samples[i];
                    cache.observe(sample,captured.camera.cameraPosition,captured.config->nearbyDistance,
                        captured.config->retainLastPosition,captured.config->showAllActors);
                }
                batch=captured;
            }
            if(batch) lastCapture=batch;
            else if(lastCapture) { batch=lastCapture; batch->count=0; }
            if(!batch) continue;
            nextPublication=Clock::now()+std::chrono::milliseconds(50);
            const auto& config=*batch->config;
            auto snapshot = std::make_shared<RenderSnapshot>();
            snapshot->epoch = batch->camera.epoch;
            snapshot->liveLifetime = batch->liveLifetime;
            snapshot->boxes = cache.visible(Clock::now(), batch->camera.cameraPosition,
                config.nearbyDistance, config.retainLastPosition,
                batch->liveLifetime, config.showAllActors, config.playersOnly,
                std::chrono::minutes(config.retentionMinutes));
            for(auto& box:snapshot->boxes) {
                box.blendDuration=std::chrono::milliseconds(0);
                box.blendFrom=box.bounds;
            }
            if(snapshot->boxes.empty() && publishedEmpty) continue;
            const auto revision=nameMailbox.revision();
            const bool mailboxChanged=revision!=mailboxRevision;
            mailboxRevision=revision;
            std::size_t observedNames=0,readyMeshes=0,misses=0,blanks=0,failures=0,savedBoxes=0,savedMeshes=0;
            if(config.showNameTags) {
                snapshot->nameMeshes.reserve(snapshot->boxes.size());
                std::size_t built=0;
                const auto fontBegin=Clock::now();
                for(const auto& box:snapshot->boxes) {
                    if(box.retained) ++savedBoxes;
                    const hitbox::NameKey key{batch->camera.epoch,box.runtimeId,box.actorKey,box.entityId};
                    auto found=names.find(box.runtimeId);
                    const auto archived=playerNames.read(key,box.uniqueId);
                    auto observed=nameMailbox.readForRuntime(key);
                    const auto nowName=Clock::now();
                    const auto nameLifetime=std::chrono::minutes(config.nameCacheMinutes);
                    if(observed && nowName-observed->observedAt>=nameLifetime)observed.reset();
                    const bool fromArchive=!observed && archived.has_value();
                    if(fromArchive)observed=archived;
                    if(found!=names.end() && nowName>=found->second.expires && !observed) {
                        names.erase(found);found=names.end();
                    }
                    if(observed && found!=names.end())
                        found->second.expires=observed->observedAt+nameLifetime;
                    if(observed || (found!=names.end() && !found->second.text.empty())) ++observedNames; else ++misses;
                    if(observed && (found==names.end() || found->second.key!=key ||
                        found->second.revision!=observed->revision || found->second.archived!=fromArchive)) {
                        auto text=hitbox::normalizedName(std::string_view(observed->bytes.data(),observed->size));
                        if(text.empty()) ++blanks;
                        if(!text.empty()) {
                            playerNames.remember(key,text,observed->observedAt);
                            if(found==names.end() && names.size()>=hitbox::TrackingBudget::maximumActors) {
                                const auto oldest=std::min_element(names.begin(),names.end(),[](const auto& a,const auto& b) {
                                    return a.second.touched<b.second.touched;
                                });
                                names.erase(oldest);
                            }
                            auto& entry=names[box.runtimeId];
                            entry.key=key; entry.revision=observed->revision;entry.archived=fromArchive;
                            entry.expires=observed->observedAt+nameLifetime;
                            if(entry.text!=text) { entry.text=std::move(text); entry.retryAfter={}; }
                            found=names.find(box.runtimeId);
                            if(Clock::now()>=nextNameExample) {
                                nextNameExample=Clock::now()+std::chrono::seconds(5);
                                log("NameTag cached: shard=",shard," epoch=",key.epoch," runtime_id=",box.runtimeId,
                                    " retained=",box.retained," source=",fromArchive?"playerlist_uuid_cache":"actor_nametag"," name=",entry.text);
                            }
                        } else if(found!=names.end()) {
                            found->second.key=key; found->second.revision=observed->revision;
                        }
                    }
                    if(found!=names.end()) {
                        auto& entry=found->second;
                        // RuntimeID + world epoch survives Actor disappearance. A
                        // copied name must not depend on reading the old native Actor again.
                        if(!entry.text.empty() && (!entry.mesh || entry.mesh->text!=entry.text) &&
                            built<2 && Clock::now()>=entry.retryAfter &&
                            (!built || Clock::now()-fontBegin<std::chrono::microseconds(1500))) {
                            ++built;
                            auto mesh=nameMesher.make(entry.text);
                            if(mesh) {
                                entry.mesh=std::move(mesh); entry.retryAfter={};
                                if(!nameReadyLogged) {
                                    nameReadyLogged=true;
                                    log("HitBox name mesh ready: worker=",shard," runtime_id=",box.runtimeId,
                                        " name=",entry.mesh->text," [cached text; live + retained]");
                                }
                            } else {
                                ++failures; entry.retryAfter=Clock::now()+std::chrono::seconds(1);
                                if(!fontFailureLogged) {
                                    fontFailureLogged=true;
                                    log("NameTag mesh deferred: shard=",shard," runtime_id=",box.runtimeId,
                                        " stage=",nameMesher.lastStage()," hresult=",static_cast<std::uint32_t>(nameMesher.lastResult()),
                                        " [name preserved; retry in 1s]");
                                }
                            }
                        }
                        entry.touched=++nameTouch;
                        if(entry.mesh) { ++readyMeshes; if(box.retained) ++savedMeshes; }
                    }
                    snapshot->nameMeshes.push_back(found==names.end() ? nullptr : found->second.mesh);
                }
            }
            const auto preparationBegin=Clock::now();
            bool reused=false;
            if(config.showNameTags) {
                const auto origin=renderCamera.read(snapshot->epoch);
                const auto height=0.25f*config.nameTagScale;
                if(origin && previousPrepared && previousPrepared->preparedNames &&
                    hitbox::canReuseNames(*previousPrepared->preparedNames,
                        previousPrepared->boxes,previousPrepared->nameMeshes,snapshot->boxes,snapshot->nameMeshes,
                        *origin,preparationBegin,height)) {
                    snapshot->preparedNames=previousPrepared->preparedNames;
                    reused=true;
                } else if(origin) {
                    auto prepared=std::make_shared<hitbox::PreparedNames>(hitbox::prepareNames(
                        snapshot->boxes,snapshot->nameMeshes,*origin,preparationBegin,height,98304/workerCount));
                    nameDiagnostics.renderBudget+=prepared->budgetSkipped;
                    snapshot->preparedNames=std::move(prepared);
                }
            }
            if(!reused) previousPrepared=snapshot;
            nameDiagnostics.workers[shard].geometryReused=reused;
            nameDiagnostics.workers[shard].vertices=snapshot->preparedNames ? snapshot->preparedNames->vertices.size() : 0;
            nameDiagnostics.workers[shard].prepareUs=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-preparationBegin).count();
            nameDiagnostics.workers[shard].boxes=snapshot->boxes.size();
            nameDiagnostics.workers[shard].observed=observedNames;
            nameDiagnostics.workers[shard].meshes=readyMeshes;
            nameDiagnostics.workers[shard].retainedBoxes=savedBoxes;
            nameDiagnostics.workers[shard].retainedMeshes=savedMeshes;
            nameDiagnostics.mailboxMisses+=misses;
            nameDiagnostics.blankNames+=blanks;
            nameDiagnostics.meshFailures+=failures;
            if (snapshot->epoch != worldEpoch.load(std::memory_order_acquire)) continue;
            const auto previous = std::atomic_load_explicit(&worker.snapshot, std::memory_order_acquire);
            if (previous) retired.push_back(previous);
            publishedEmpty=snapshot->boxes.empty();
            std::atomic_store_explicit(&worker.snapshot,
                std::shared_ptr<const RenderSnapshot>(std::move(snapshot)), std::memory_order_release);
            if (config.debug && Clock::now() >= nextLog) {
                nextLog = Clock::now() + std::chrono::seconds(10);
                log("HitBox worker ", shard, ": native_pool=", batch->population,
                    " copied=", batch->count, " cached=", cache.size(), " last_position_records=", cache.retainedSize(), " name_cache=", names.size(), " all_actors=", config.showAllActors, " players_only=", config.playersOnly, " tracked_total=", trackingBudget->size(), " tracking_limit=", hitbox::TrackingBudget::maximumActors, " [RuntimeID partition]");
            }
        } catch (...) { ++nameDiagnostics.workerExceptions; }
    }
}
void stopProcessingWorkers() noexcept {
    for (auto& worker : processingWorkers) {
        { std::lock_guard lock(worker.mutex); worker.stopping = true; worker.pending.clear(); }
        worker.ready.notify_one();
    }
    for (auto& worker : processingWorkers) if (worker.thread.joinable()) worker.thread.join();
}
bool startProcessingWorkers() noexcept {
    const auto logicalCpus=std::thread::hardware_concurrency();
    workerCount=std::clamp<std::size_t>(logicalCpus>2 ? logicalCpus-2 : 1,1,workerCapacity);
    try {
        for (std::size_t i = 0; i < workerCount; ++i)
            processingWorkers[i].thread = std::thread(processingLoop, i);
        return true;
    } catch (...) { stopProcessingWorkers(); return false; }
}
void refreshGameTickSnapshot() {
    static void* snapshotWorld = nullptr;
    static std::uint64_t snapshotEpoch = 0;
    static std::uintptr_t snapshotRegistry = 0;
    static void* snapshotClient = nullptr;
    static std::uint64_t snapshotRuntime = 0;
    static void* snapshotLocal = nullptr;
    static std::uint32_t snapshotEntity = 0;
    static auto lastTick = Clock::time_point{};
    static std::size_t cursor = 0;
    static hitbox::CaptureSchedule schedule;
    static hitbox::PendingDelivery<CaptureBatch,workerCapacity> delivery;
    static auto nextReport = Clock::time_point{};
    static auto nextFailureReport = Clock::time_point{};
    static double averageVisited = maximumNativeBatch;
    static std::array<std::size_t,9> diagnostics{};
    static bool loggedNativeReady = false;
    static bool loggedCameraWaiting = false;
    static bool loggedNativeFailure = false;
    auto config = std::atomic_load_explicit(&liveConfig, std::memory_order_acquire);
    if (!config->enabled || stopRequested.load(std::memory_order_acquire)) return;
    auto* world = currentWorld.load(std::memory_order_acquire);
    if (!world) return;
    const auto now = Clock::now();
    // Pointer addresses may be reused after leaving/rejoining a world.
    const bool interrupted=lastTick!=Clock::time_point{} && now-lastTick>std::chrono::seconds(2);
    lastTick=now;
    if (snapshotWorld != world || snapshotClient!=access.clientIdentity() || interrupted ||
        snapshotEpoch!=worldEpoch.load(std::memory_order_acquire)) {
        snapshotWorld = world;
        snapshotClient=access.clientIdentity(); snapshotRuntime=0;
        snapshotRegistry = 0;
        snapshotLocal=nullptr; snapshotEntity=0;
        localActorIdentity.store(0,std::memory_order_release);
        access.setWorld(world);
        access.resetPlayerTracking();
        diagnostics={};
        worldEpoch.fetch_add(1, std::memory_order_acq_rel);
        cursor = 0;
        schedule.reset();
        delivery.clear();
        averageVisited = maximumNativeBatch;
        loggedNativeReady = loggedCameraWaiting = loggedNativeFailure = false;
        snapshotEpoch=worldEpoch.load(std::memory_order_acquire);
        log("world session reset: epoch=",snapshotEpoch," level=",world," client=",snapshotClient,
            " tick_interrupted=",interrupted," renderer_boundaries=",renderBoundaryChanges.load(),
            " [Player IDs, retained boxes, names, prepared geometry and queued old epochs invalidated]");
    }
    gameHeartbeatMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count(), std::memory_order_release);
    const auto sendBatch=[](std::size_t shard,const CaptureBatch& batch) {
        if(shard>=workerCount) return true;
        auto& worker=processingWorkers[shard];
        std::unique_lock lock(worker.mutex,std::try_to_lock);
        if(!lock || worker.stopping) return false;
        CaptureBatch partition=batch;
        partition.count=0;
        for(std::size_t i=0;i<batch.count;++i)
            if(actorWorker(batch.samples[i].runtimeId)==shard)
                partition.samples[partition.count++]=batch.samples[i];
        if(!worker.pending.push(partition)) return false;
        lock.unlock(); worker.ready.notify_one(); return true;
    };
    NativeReadWindow readWindow;
    auto* local = access.localPlayer();
    const auto localReadyAt = Clock::now();
    const auto registry=local ? access.registryIdentity(local) : 0;
    std::uint32_t entity=0;
    const bool identityReady=local && registry && access.copyNameEntityId(local,entity);
    if (!identityReady) {
        localActorIdentity.store(0,std::memory_order_release);
        if(snapshotRegistry) {
            worldEpoch.fetch_add(1,std::memory_order_acq_rel);
            snapshotEpoch=worldEpoch.load(std::memory_order_acquire);
            snapshotRegistry=0; snapshotLocal=nullptr; snapshotEntity=0;
            cursor=0; schedule.reset(); delivery.clear(); access.resetPlayerTracking();
            log("local world unavailable; saved positions cleared");
        }
        return;
    }
    const auto runtime=access.runtimeIdentity(local);
    if (snapshotRegistry && (registry!=snapshotRegistry || local!=snapshotLocal || entity!=snapshotEntity ||
        (snapshotRuntime && runtime && snapshotRuntime!=runtime))) {
        worldEpoch.fetch_add(1,std::memory_order_acq_rel);
        cursor=0; schedule.reset(); delivery.clear();
        access.resetPlayerTracking();
        diagnostics={};
        averageVisited=maximumNativeBatch;
        loggedNativeReady=loggedCameraWaiting=loggedNativeFailure=false;
        snapshotEpoch=worldEpoch.load(std::memory_order_acquire);
        log("world identity replaced; Player identity and last-position records cleared");
    }
    snapshotRegistry=registry;
    if(runtime) snapshotRuntime=runtime;
    snapshotLocal=local; snapshotEntity=entity;
    localActorIdentity.store(reinterpret_cast<std::uintptr_t>(local),std::memory_order_release);
    // Validate the world before delivering any previously captured rows.
    const bool workerReady=delivery.flush(sendBatch);
    // All-Actor mode does not need local position for distance filtering.
    const auto cameraPosition = config->showAllActors
        ? std::optional<bedrock::Vec3>{bedrock::Vec3{}} : access.actorPosition(local);
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
    const bool captured=access.captureBatch(local, cursor, schedule, batch, visited);
    if (!captured || batch.captureFailure) {
        if (!loggedNativeFailure || now>=nextFailureReport) {
            loggedNativeFailure=true;
            nextFailureReport=now+std::chrono::seconds(10);
            log("Actor capture waiting: reason=", batch.captureFailure ? batch.captureFailure : "unknown",
                " stage=", batch.captureStage, " exception_code=", batch.faultCode,
                " exception_address=", reinterpret_cast<void*>(batch.faultAddress),
                " registry=", reinterpret_cast<void*>(registry), " pool=", batch.population,
                " visited=", visited, " copied=", batch.count, " [will retry on next tick]");
        }
        if (!captured) {access.refreshTrackedPoses(local,batch);return;}
    } else if (loggedNativeFailure) {
        loggedNativeFailure=false;
        log("Actor capture recovered: pool=", batch.population, " copied=", batch.count);
    }
    access.capturePlayerNames(world,batch.camera.epoch,config->nameCacheMinutes);
    // Native position capture is one game tick for every tracked valid handle.
    access.refreshTrackedPoses(local,batch);
    fillMissingName(batch);
    const auto actorsReadyAt = Clock::now();
    const std::array<std::size_t,9> counts{visited,batch.count,batch.nativePlayers,
        batch.rememberedPlayers,batch.filteredActors,batch.missingRuntime,
        batch.invalidOwner,batch.missingShape,batch.invalidBounds};
    for (std::size_t i=0;i<counts.size();++i) diagnostics[i]+=counts[i];
    if (visited) averageVisited = averageVisited * 0.9 + static_cast<double>(visited) * 0.1;
    if (batch.count && !loggedNativeReady) {
        loggedNativeReady = true;
        log("native incremental capture active: pool=", batch.population,
            " [RuntimeID + AABB copies; 1-tick sweep target; maximum 32 rows/tick; 250us budget]");
    }
    // Keep live records across a full incremental pass, even if the time budget
    // reduced the number of rows captured on a busy callback.
    batch.liveLifetime = std::chrono::milliseconds(std::clamp<std::size_t>(
        batch.population * 100 / std::max<std::size_t>(1, static_cast<std::size_t>(averageVisited)),
        750, 30000));
    if(workerReady) { delivery.begin(batch); delivery.flush(sendBatch); }
    // Worker backpressure may defer retention/name work, never fresh poses.
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-now).count();
    if ((config->debug || elapsed >= 2000) && now >= nextReport) {
        nextReport = now + std::chrono::seconds(10);
        const auto us = [](auto from, auto to) {
            return std::chrono::duration_cast<std::chrono::microseconds>(to - from).count();
        };
        log("HitBox native capture us=", elapsed,
            " local=", us(now, localReadyAt), " position=", us(localReadyAt, positionReadyAt),
            " actors=", us(positionReadyAt, actorsReadyAt),
            " tick_poses=",batch.tickPoseCount," tick_pose_us=",batch.tickPoseUs,
            " publish=", us(actorsReadyAt, Clock::now()), " pool=", batch.population,
            " visited=", visited, " copied=", batch.count,
            " workers=",workerCount," [1-tick sweep target; maximum 32 rows/tick; cooperative 250us actor budget]");
        if(config->debug) log("HitBox capture observations since last report: visited=", diagnostics[0],
            " copied=", diagnostics[1], " native_player=", diagnostics[2],
            " remembered_player=", diagnostics[3], " filtered_non_player=", diagnostics[4],
            " missing_runtime=", diagnostics[5], " invalid_owner=", diagnostics[6],
            " missing_shape=", diagnostics[7], " invalid_or_zero_bounds=", diagnostics[8],
            " [observations, not distinct Actors; missing Actors cannot be read]");
        diagnostics={};
    }
}
void observeWorld(const bedrock::LevelTickEvent& event) {
    if (!event.level) {
        if(currentWorld.exchange(nullptr,std::memory_order_acq_rel))
            worldEpoch.fetch_add(1,std::memory_order_acq_rel);
        return;
    }
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
using ColorFn = void(__fastcall*)(void*, float, float, float, float);
using MeshFn = void(__fastcall*)(void*, void*, void*, void*);
using HashedNameCtorFn = void*(__fastcall*)(void*, const char*);
using MaterialCtorFn = void*(__fastcall*)(void*, void*, const void*);
HashedNameCtorFn hashedNameCtor{};
MaterialCtorFn materialCtor{};
void* commonMaterialGroup{};
// Native constructors own any internal allocations. Keep these two constructed
// objects for the pinned DLL lifetime; never run another STL's destructor on them.
alignas(16) std::array<std::byte,48> overlayNameStorage{};
alignas(16) std::array<std::byte,16> overlayMaterialStorage{};
std::atomic<int> overlayMaterialState{}; // 0 waiting, 1 constructing, 2 ready, -1 fault
int ensureOverlayMaterial() noexcept {
    const auto state=overlayMaterialState.load(std::memory_order_acquire);
    if (state==2 || state==-1) return state==2 ? 1 : -1;
    void* vtable=nullptr; void* factory=nullptr;
    if (!safeRead(commonMaterialGroup,0,vtable) || !vtable ||
        !safeRead(vtable,sizeof(void*),factory) || !isExecutable(factory)) return 0;
    int expected=0;
    if (!overlayMaterialState.compare_exchange_strong(expected,1,std::memory_order_acq_rel)) return 0;
    __try {
        hashedNameCtor(overlayNameStorage.data(),"name_tag");
        materialCtor(overlayMaterialStorage.data(),commonMaterialGroup,overlayNameStorage.data());
        void* info=nullptr;
        if (!safeRead(overlayMaterialStorage.data(),0,info) || !info) {
            overlayMaterialState.store(-1,std::memory_order_release); return -1;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        overlayMaterialState.store(-1,std::memory_order_release); return -1;
    }
    overlayMaterialState.store(2,std::memory_order_release); return 1;
}
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
constexpr std::size_t boxesPerMesh = 512; // Submission size, never a total visibility limit.
struct NativeDrawRange { const BoxRecord* boxes{}; std::size_t count{};
    const std::uint8_t* visible{};
    const hitbox::PreparedNames* names{};
    std::chrono::milliseconds liveLifetime{};
    const BoxRecord* preparedBoxes{}; };
struct NativeDrawInput {
    std::array<NativeDrawRange, workerCapacity> ranges{};
    std::array<std::uint8_t,hitbox::TrackingBudget::maximumActors> visible{};
    std::uintptr_t localActor{};
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

struct NameQuadBatch {
    void* screen{};
    void* tess{};
    void* shader{};
    FloatColor color{};
    std::size_t remaining{},inMesh{};
    bool failed{};
    void flush() noexcept {
        if(!inMesh || failed) return;
        // Each native mesh submission gets its own uniform update. Do not rely
        // on state surviving a previous engine submission or mesh split.
        const std::uint8_t dirty=1;
        if(!safeWrite(shader,0,color) || !safeWrite(shader,worldRenderProfile.dirty,dirty)) {
            failed=true; ++nameDiagnostics.renderFaults; return;
        }
        alignas(16) std::array<std::byte,0x58> scratch{};
        renderMesh(screen,tess,overlayMaterialStorage.data(),scratch.data());
        inMesh=0;
    }
    void quad(const std::array<bedrock::Vec3,4>& vertices) noexcept {
        if(failed) return;
        if(!inMesh) tessBegin(tess,0,1,static_cast<int>(std::min<std::size_t>(remaining,16384)),false);
        for(const auto& p:vertices) tessVertex(tess,p.x,p.y,p.z);
        inMesh+=4; remaining-=4;
        if(inMesh==16384 || !remaining) flush();
    }
};
bool drawNativeNames(void* screen,void* tess,void* shader,const bedrock::Vec3& origin,
                     const NativeDrawInput* input) noexcept {
    ++nameDiagnostics.renderCalls;
    std::size_t submitted=0,missing=0,hidden=0;
    for(const auto& range:input->ranges) {
        if(!range.names) { missing+=range.count; continue; }
        const auto& prepared=*range.names;
        missing+=range.count-std::min(range.count,prepared.labels.size());
        // Vertices were transformed on a worker against a copied render origin.
        // Native Tessellator::vertex applies this constant translation; no label
        // triangle/billboard construction happens in the render callback.
        const bedrock::Vec3 delta{prepared.origin.x-origin.x,prepared.origin.y-origin.y,prepared.origin.z-origin.z};
        if(!safeWrite(tess,worldRenderProfile.translation,delta)) { ++nameDiagnostics.renderFaults; return false; }
        for(unsigned pass=0;pass<2;++pass) {
            std::size_t vertices=0;
            for(const auto& label:prepared.labels) {
                if(label.boxIndex>=range.count) continue;
                const auto& box=range.boxes[label.boxIndex];
                if(!range.visible[label.boxIndex]) { if(!pass) ++hidden; continue; }
                if(box.retained==(pass==1)) vertices+=label.vertexCount;
            }
            if(!vertices) continue;
            const auto rgba=hitbox::opaqueColor(input->config.retainedColor);
            const FloatColor color=pass==0 ? FloatColor{1,1,1,1} : FloatColor{rgba[0],rgba[1],rgba[2],1};
            NameQuadBatch batch{screen,tess,shader,color,vertices};
            for(const auto& label:prepared.labels) {
                if(label.boxIndex>=range.count || label.firstVertex>prepared.vertices.size() ||
                    label.vertexCount>prepared.vertices.size()-label.firstVertex) continue;
                const auto& box=range.boxes[label.boxIndex];
                if(box.retained!=(pass==1) || !range.visible[label.boxIndex]) continue;
                // Text glyphs stay cached, but their anchor follows the same
                // newest copied AABB as the outline, without font rebuilds.
                bedrock::Vec3 translation=delta;
                if(range.preparedBoxes) {
                    const auto before=hitbox::interpolatedBounds(range.preparedBoxes[label.boxIndex],prepared.preparedAt);
                    const auto after=hitbox::interpolatedBounds(box,input->now);
                    translation.x+=(after.minimum.x+after.maximum.x-before.minimum.x-before.maximum.x)*0.5f;
                    translation.y+=after.maximum.y-before.maximum.y;
                    translation.z+=(after.minimum.z+after.maximum.z-before.minimum.z-before.maximum.z)*0.5f;
                }
                if(!safeWrite(tess,worldRenderProfile.translation,translation)) return false;
                for(std::size_t p=label.firstVertex;p<label.firstVertex+label.vertexCount;p+=4)
                    batch.quad({prepared.vertices[p],prepared.vertices[p+1],prepared.vertices[p+2],prepared.vertices[p+3]});
                ++submitted;
            }
            batch.flush();
            if(batch.failed) return false;
        }
    }
    nameDiagnostics.renderNoMesh+=missing;
    nameDiagnostics.renderHidden+=hidden;
    nameDiagnostics.renderSubmitted+=submitted;
    return true;
}

int drawNativeGuarded(void* renderer, void* screen, const NativeDrawInput* input) noexcept {
    // No owning C++ objects inside SEH. Snapshots remain owned by drawWorldBoxes.
    const auto& p=worldRenderProfile;
    void* player=nullptr; void* tess=nullptr; void* shader=nullptr; void* materialInfo=nullptr;
    bedrock::Vec3 origin{}, oldTranslation{};
    FloatColor oldColor{};
    std::uint8_t building=0, special=0;
    if (!safeRead(renderer,p.player,player) || !player ||
        !safeRead(player,p.origin,origin) || !std::isfinite(origin.x) ||
        !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
        !safeRead(screen,p.tess,tess) || !tess ||
        !safeRead(screen,p.shader,shader) || !shader ||
        !safeRead(tess,p.building,building) || building ||
        !safeRead(tess,p.special,special) || special ||
        !safeRead(tess,p.translation,oldTranslation) || !safeRead(shader,0,oldColor)) return 0;
    const auto materialReady=ensureOverlayMaterial();
    if (materialReady<=0) return materialReady;
    if (!safeRead(overlayMaterialStorage.data(),0,materialInfo) || !materialInfo ||
        !permittedAddress(materialInfo,sizeof(void*))) return -1;
    const bedrock::Vec3 zero{};
    const std::uint8_t dirty=1;
    if (!safeWrite(tess,p.translation,zero)) return 0;
    bool success=true;
    __try {
        // name_tag uses Position + current_color, depth Always, no depth writes.
        // Draw every copied Actor; split each color into bounded mesh submissions.
        std::size_t total=0;
        for (const auto& range:input->ranges) total+=range.count;
        for (unsigned pass=0;pass<2;++pass) {
            std::size_t visited=0, count=0;
            for (const auto& range:input->ranges) {
                for (std::size_t i=0;i<range.count && visited<total;++i,++visited) {
                    if(!range.visible[i]) continue;
                    const bool retained=range.boxes[i].retained;
                    if (retained==(pass==1)) ++count;
                }
            }
            if (!count) continue;
            const auto rgba=hitbox::opaqueColor(pass ? input->config.retainedColor : input->config.liveColor);
            const FloatColor color{rgba[0],rgba[1],rgba[2],rgba[3]};
            std::size_t remaining=count, inMesh=0;
            visited=0;
            for (const auto& range:input->ranges) {
                for (std::size_t i=0;i<range.count && visited<total;++i,++visited) {
                    const auto& box=range.boxes[i];
                    if(!range.visible[i]) continue;
                    const bool retained=box.retained;
                    if (retained!=(pass==1)) continue;
                    if(!inMesh) {
                        if(!safeWrite(shader,0,color) || !safeWrite(shader,p.dirty,dirty)) { success=false; break; }
                        tessBegin(tess,0,4,static_cast<int>(std::min(remaining,boxesPerMesh)*24),false);
                    }
                    const auto vertices=hitbox::worldLineVertices(box,origin,input->now);
                    for (const auto& v:vertices) tessVertex(tess,v.x,v.y,v.z);
                    ++inMesh; --remaining;
                    if(inMesh==boxesPerMesh || !remaining) {
                        alignas(16) std::array<std::byte,0x58> scratch{};
                        renderMesh(screen,tess,overlayMaterialStorage.data(),scratch.data());
                        inMesh=0;
                    }
                }
                if(!success) break;
            }
            if(!success) break;
        }
        if(success && input->config.showNameTags)
            success=drawNativeNames(screen,tess,shader,origin,input);
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
        gameHeartbeatMs.load(std::memory_order_acquire)>1000) { ++nameDiagnostics.frameStale; return; }
    const auto epoch=worldEpoch.load(std::memory_order_acquire);
    std::array<std::shared_ptr<const RenderSnapshot>,workerCapacity> snapshots;
    NativeDrawInput input; input.config=*config; input.now=now;
    input.localActor=localActorIdentity.load(std::memory_order_acquire);
    // Fixed TLS storage: no per-frame allocation and no native Actor reads.
    static thread_local std::array<BoxRecord,hitbox::TrackingBudget::maximumActors> frameBoxes;
    std::size_t count=0;
    for (std::size_t i=0;i<workerCount;++i) {
        snapshots[i]=std::atomic_load_explicit(&processingWorkers[i].snapshot,std::memory_order_acquire);
        if (!snapshots[i] || snapshots[i]->epoch!=epoch) continue;
        // Old and new worker snapshots can overlap during slot handover.
        // Bound the combined frame even during that asynchronous transition.
        const auto size=std::min(snapshots[i]->boxes.size(),hitbox::TrackingBudget::maximumActors-count);
        auto* current=frameBoxes.data()+count;
        std::copy_n(snapshots[i]->boxes.data(),size,current);
        latestPoses.apply(epoch,current,size);

        input.ranges[i]={current,size,input.visible.data()+count,
            snapshots[i]->preparedNames.get(),snapshots[i]->liveLifetime,snapshots[i]->boxes.data()};
        // Evaluate identity/expiry once, shared by line and both text passes.
        for(std::size_t j=0;j<input.ranges[i].count;++j) {
            const auto& box=input.ranges[i].boxes[j];
            input.visible[count+j]=box.actorKey!=input.localActor &&
                hitbox::visibleInFrame(box,now,snapshots[i]->liveLifetime,config->retainLastPosition,
                    config->playersOnly,std::chrono::minutes(config->retentionMinutes));
        }
        count+=input.ranges[i].count;
    }
    if (!count || worldEpoch.load(std::memory_order_acquire)!=epoch) { ++nameDiagnostics.frameNoBoxes; return; }
    NativeReadWindow window;
    const auto renderBegin=Clock::now();
    const auto result=drawNativeGuarded(renderer,screen,&input);
    const auto renderUs=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-renderBegin).count());
    ++nameDiagnostics.renderFrames; nameDiagnostics.renderUs+=renderUs;
    auto peak=nameDiagnostics.renderPeakUs.load(std::memory_order_relaxed);
    while(peak<renderUs && !nameDiagnostics.renderPeakUs.compare_exchange_weak(peak,renderUs,std::memory_order_relaxed)) {}
    static std::atomic<bool> loggedDraw{};
    if (result>0 && !loggedDraw.exchange(true))
        log("native world mesh submitted [12 edges/box; engine render origin + name_tag; depth Always; opaque uniform colors; interpolated]");
    if (result<0 && !worldRenderFault.exchange(true))
        log("world rendering disabled after guarded native fault [restart required]");
}
void observeRenderContext(void* renderer) {
    static void* previousRenderer=nullptr;
    static void* previousPlayer=nullptr;
    void* player=nullptr;
    bedrock::Vec3 origin{};
    const bool ready=safeRead(renderer,worldRenderProfile.player,player) && player &&
        safeRead(player,worldRenderProfile.origin,origin) && std::isfinite(origin.x) &&
        std::isfinite(origin.y) && std::isfinite(origin.z);
    if(!ready) {
        if(previousRenderer) {
            previousRenderer=previousPlayer=nullptr;
            ++renderBoundaryChanges; worldEpoch.fetch_add(1,std::memory_order_acq_rel);
        }
        return;
    }
    if(previousRenderer && (renderer!=previousRenderer || player!=previousPlayer)) {
        ++renderBoundaryChanges; worldEpoch.fetch_add(1,std::memory_order_acq_rel);
    }
    previousRenderer=renderer; previousPlayer=player;
    renderCamera.publish(worldEpoch.load(std::memory_order_acquire),origin);
}
void BDS_HOOK_CALL renderLevelSubscriber(void*, void* raw, BDS_HookNext next, void* frame) {
    auto& args=*static_cast<BDS_RenderLevelArgs*>(raw);
    next(frame,raw);
    void* renderer=args.renderer; void* screen=args.screen;
    // The native world pass supplies view/projection matrices, origin and depth.
    static thread_local bool drawing=false;
    if (drawing) return;
    drawing=true;
    try { observeRenderContext(renderer); drawWorldBoxes(renderer,screen); }
    catch (...) { if(!worldRenderFault.exchange(true)) log("world rendering disabled after exception"); }
    drawing=false;
}
bool installWorldRenderer(std::string& error) {
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::array<void*,7> addresses{};
    for (std::size_t i=0;i<addresses.size();++i) {
        const auto& f=worldRenderProfile.functions[i];
        auto* target=reinterpret_cast<const std::byte*>(base+f.rva);
        if (!isExecutable(target)) { error="world render target is not executable"; return false; }
        if (i==0 && BDS_HookOwnsTarget(BDS_HOOK_RENDER_LEVEL,const_cast<std::byte*>(target))) {
            addresses[i]=const_cast<std::byte*>(target); continue;
        }
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
    hashedNameCtor=reinterpret_cast<HashedNameCtorFn>(addresses[5]);
    materialCtor=reinterpret_cast<MaterialCtorFn>(addresses[6]);
    commonMaterialGroup=reinterpret_cast<void*>(base+worldRenderProfile.commonMaterialGroup);
    return BDS_HookSubscribe(BDS_HOOK_RENDER_LEVEL,addresses[0],&renderLevelSubscriber,nullptr,&renderHookToken)!=0 ||
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
    log("build ", BEDROCK_HITBOX_BUILD_ID, " native ABI=Microsoft C++; compiler=", _MSC_VER,
        " iterator_debug=", _ITERATOR_DEBUG_LEVEL);
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
    std::size_t playerListOffset{};
    const auto identity = choice.identity;
    const auto prepareProfile = [&](std::string_view candidate) {
        // Profile selection checks executable identity, but an older cache can
        // still lack layouts required by a newer mod. Validate the whole set
        // before accepting it or installing any native hooks.
        bedrock::Resolver candidateResolver;
        try {
            playerListOffset=0;
            const auto root=bedrock::parseNativeProfile(candidate);
            if(root.contains("native_player_identity")) {
                const auto& data=root.at("native_player_identity");
                if(data.at("schema")!=1 || data.at("pointer_size")!=8 || data.at("entry_size")!=0xA8 ||
                   data.at("actor_unique_id_offset")!=0 || data.at("uuid_offset")!=8 || data.at("name_offset")!=24)
                    throw std::runtime_error("unsupported native player identity layout");
                playerListOffset=data.at("level_player_list_offset").get<std::size_t>();
                if(!playerListOffset || playerListOffset>0x10000)throw std::runtime_error("invalid PlayerList offset");
            }
        } catch(const std::exception& ex){error=ex.what();return false;}
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
        if (!bedrock::parseNativeWorldEventsLayout(candidate, error)) return false;
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
        profile = bedrock::generated::kSymbolDatabase;
        profileReady = true;
        log("native layouts recovered from embedded exact-build profile");
    }
    if (!profileReady) {
        log("exact-build native Actor profile unavailable: ", error);
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    access.setPlayerListOffset(playerListOffset);
    if(!playerListOffset)log("PlayerList UUID roster unavailable; ActorType/RuntimeID filtering and Actor NameTag capture remain active");
    access.setLayouts(std::move(itemLayout), std::move(actorLayout),
                      worldRenderProfile.actorTypeValueOffset, worldRenderProfile.playerTypeId);
    log("native Actor registry source ready [all valid RuntimeID/AABB Actors including Players; ActorType + remembered RuntimeID player filter; no name filter; profile candidate]");

    if (!startProcessingWorkers()) { status.store(Status::Failed); return 1; }
    struct WorkerStop { ~WorkerStop() { stopProcessingWorkers(); } } workerStop;
    log(workerCount," processing workers + 1 log writer ready [RuntimeID partitions; 20Hz coalesced snapshots; packed text quads; copied scalars only]");
    gameHooks = std::make_unique<bedrock::GameHooks>(gameApi);
    gameApi.Event.Enum.Gameplay.then([](const bedrock::GameplayEvent& event) {
        if (event.clientInstance) access.setClient(event.clientInstance);
    });
    gameApi.Event.Enum.LevelTick.then(&observeWorld);
    gameApi.Event.Enum.WorldChange.then([](const bedrock::WorldChangeEvent& event) {
        // Workers/renderers already fence their snapshots by this generation.
        // Advancing it immediately hides old boxes, retained positions and text.
        worldEpoch.fetch_add(1, std::memory_order_acq_rel);
        currentWorld.store(nullptr, std::memory_order_release);
        access.setClient(event.clientInstance);
        log("world lifecycle event: reason=", static_cast<unsigned>(event.reason),
            " sdk_epoch=", event.epoch, " [old-world snapshots invalidated]");
    });
    if (!gameHooks->installGameplay(resolver, error) ||
        !gameHooks->installLevelTick(resolver, error) ||
        !gameHooks->installWorldEvents(resolver, profile, error)) {
        log("SDK hook registration failed: ", error);
        gameHooks.reset();
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    log("NameTag configuration: show_name_tags=",std::atomic_load(&liveConfig)->showNameTags,
        " name_tag_scale=",std::atomic_load(&liveConfig)->nameTagScale);
    const auto nameSource=bedrock::resolveSharedHook(resolver,"Actor::getNameTag",BDS_HOOK_ACTOR_GET_NAME_TAG);
    log("NameTag resolved source: exact_build=",resolver.exactBuild()," target=",
        nameSource ? reinterpret_cast<void*>(nameSource->address) : nullptr,
        " usable=",nameSource && bedrock::isUsableSharedHookResolution(*nameSource));
    if(resolver.exactBuild() && nameSource && bedrock::isUsableSharedHookResolution(*nameSource))
        nameGetter.store(reinterpret_cast<void*>(nameSource->address),std::memory_order_release);
    if(nameSource && bedrock::isUsableSharedHookResolution(*nameSource) &&
        BDS_HookSubscribe(BDS_HOOK_ACTOR_GET_NAME_TAG,reinterpret_cast<void*>(nameSource->address),
            &observeNameTag,nullptr,&nameTagToken))
        log("NameTag observation active [RuntimeID cache; missing-name fallback <=3/tick + cooperative 200us budget; worker font geometry]");
    else log("NameTag source unavailable; HitBox lines remain active");
    struct NameHookStop { ~NameHookStop() {
        if(nameTagToken) BDS_HookUnsubscribe(std::exchange(nameTagToken,nullptr));
    } } nameHookStop;
    if (!installWorldRenderer(error)) {
        log("world renderer install failed: ", error);
        gameHooks.reset();
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    log("native world rendering active [LevelRenderer::renderLevel + name_tag; depth Always; 1-tick target sweep; exact-build guards; runtime candidate]");
    log(BDS_HookRuntimeDiagnostic());
    log("ready ", choice.version, " [native Actors]");
    status.store(Status::Running, std::memory_order_release);

    auto lastWrite = std::filesystem::last_write_time(cfgPath, ec);
    while (!stopRequested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        reportNameDiagnostics();
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
