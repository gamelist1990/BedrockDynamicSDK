#include <bedrock/sdk.hpp>
#ifdef _WIN32
#if !defined(_MSC_VER)
#error "HitBox native EnTT access requires the Microsoft C++ ABI (MSVC or clang-cl)."
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#endif
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
    bedrock::sdk::Vector3 cameraPosition{};
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
    bedrock::sdk::Vector3 origin{};
    void publish(std::uint64_t value,const bedrock::sdk::Vector3& position) {
        std::unique_lock lock(mutex,std::try_to_lock);
        if(lock) { epoch=value; origin=position; }
    }
    std::optional<bedrock::sdk::Vector3> read(std::uint64_t value) {
        std::lock_guard lock(mutex);
        return epoch==value ? std::optional<bedrock::sdk::Vector3>{origin} : std::nullopt;
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

bedrock::sdk::ActorFacade actorFacade;
std::atomic<void*> hitboxClient{};
std::atomic<void*> hitboxWorld{};


std::unique_ptr<bedrock::sdk::Hooks> gameHooks;

hitbox::NameMailbox nameMailbox;
std::unique_ptr<bedrock::sdk::HookSubscription> nameTagSubscription;
std::vector<bedrock::sdk::HookSubscription> gameSubscriptions;
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
        " draw_mode=lines_and_names local_label=hidden getter=facade hook_calls=",d.hookCalls.exchange(0),
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
void observeNameTag(const bedrock::sdk::Hooks::ActorNameTagEventValue& event) noexcept {
    ++nameDiagnostics.hookCalls;
    if(stopRequested.load(std::memory_order_acquire)) return;
    try {
        const auto config=std::atomic_load_explicit(&liveConfig,std::memory_order_acquire);
        if(!config->enabled || !config->showNameTags) return;
        const auto epoch=worldEpoch.load(std::memory_order_acquire);
        if(!epoch || !event.actor || event.nameTag.empty()) return;
        std::uint32_t entity=0;
        const auto runtimeId=actorFacade.runtimeId(event.actor);
        if(!runtimeId || !(actorFacade.entityId(event.actor)&&actorFacade.entityId(event.actor).value_or(0))) { ++nameDiagnostics.hookRejected; return; }
        entity=*actorFacade.entityId(event.actor);
        ++nameDiagnostics.hookCopied;
        nameMailbox.observe({epoch,runtimeId,reinterpret_cast<std::uintptr_t>(event.actor),entity},event.nameTag);
    } catch(...) {}
}

// Only called on the game thread for a freshly validated capture sample.
// Missing names are spread over ticks; known names use the observation cache.
int copyMissingName(void* actor,char* bytes,std::size_t* count) noexcept {
    std::string name;
    std::string error;
    if(!actorFacade.nameTag(actor,name,error)) {
        if(error.find("fault")!=std::string::npos) return -1;
        return error.find("unavailable")!=std::string::npos ? -2 : 0;
    }
    if(name.empty()) return 0;
    if(name.size()>hitbox::NameMailbox::maximumBytes) name.resize(hitbox::NameMailbox::maximumBytes);
    std::memcpy(bytes,name.data(),name.size());
    *count=name.size();
    return 1;
}
void fillMissingName(const CaptureBatch& batch) {
    if(!batch.config->showNameTags || !batch.count) return;
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
        const int result=copyMissingName(reinterpret_cast<void*>(sample.actorKey),bytes.data(),&count);
        if(result==-1) {
            ++nameDiagnostics.nativeFaults;
            log("NameTag fallback native fault; observation cache remains active");
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
    if (snapshotWorld != world || snapshotClient!=hitboxClient.load(std::memory_order_acquire) || interrupted ||
        snapshotEpoch!=worldEpoch.load(std::memory_order_acquire)) {
        snapshotWorld = world;
        snapshotClient=hitboxClient.load(std::memory_order_acquire); snapshotRuntime=0;
        snapshotRegistry = 0;
        snapshotLocal=nullptr; snapshotEntity=0;
        localActorIdentity.store(0,std::memory_order_release);
        hitboxWorld.store(world,std::memory_order_release);
        ((void)0);
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
    auto* local = actorFacade.localPlayer(hitboxClient.load(std::memory_order_acquire));
    const auto localReadyAt = Clock::now();
    const auto registry=local ? ([&]()->std::uintptr_t { std::uintptr_t v=0; actorFacade.registryIdentity(local,v); return v; }()) : 0;
    std::uint32_t entity=0;
    const bool identityReady=local && registry && actorFacade.entityId(local).has_value() && ([&]{entity=actorFacade.entityId(local).value_or(0);return true;}());
    if (!identityReady) {
        if(now>=nextFailureReport){nextFailureReport=now+std::chrono::seconds(5);
            log("local Actor unavailable: client=",hitboxClient.load(std::memory_order_acquire)," actor=",local," registry=",reinterpret_cast<void*>(registry)," [exact-build getter; Actor context required]");}

        localActorIdentity.store(0,std::memory_order_release);
        if(snapshotRegistry) {
            worldEpoch.fetch_add(1,std::memory_order_acq_rel);
            snapshotEpoch=worldEpoch.load(std::memory_order_acquire);
            snapshotRegistry=0; snapshotLocal=nullptr; snapshotEntity=0;
            cursor=0; schedule.reset(); delivery.clear(); ((void)0);
            log("local world unavailable; saved positions cleared");
        }
        return;
    }
    const auto runtime=actorFacade.runtimeId(local);
    if (snapshotRegistry && (registry!=snapshotRegistry || local!=snapshotLocal || entity!=snapshotEntity ||
        (snapshotRuntime && runtime && snapshotRuntime!=runtime))) {
        worldEpoch.fetch_add(1,std::memory_order_acq_rel);
        cursor=0; schedule.reset(); delivery.clear();
        ((void)0);
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
        ? std::optional<bedrock::sdk::Vector3>{bedrock::sdk::Vector3{}}
        : ([&]() -> std::optional<bedrock::sdk::Vector3> {
              const auto native=actorFacade.position(local);
              if(!native) return std::nullopt;
              return bedrock::sdk::Vector3{native->x,native->y,native->z};
          })();
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
    bedrock::sdk::ActorFacade::ActorCaptureStatsValue stats;
    std::string captureError;
    std::vector<bedrock::sdk::ActorSample> publicSamples;
    const bool captured=actorFacade.capture(local, config->playersOnly, cursor,
        maximumNativeBatch, publicSamples, visited, stats, captureError);
    batch.count=std::min(publicSamples.size(), batch.samples.size());
    batch.population=stats.population;
    schedule.beginTick(stats.population);
    schedule.visited(visited);
    batch.nativePlayers=stats.nativePlayers;
    batch.rememberedPlayers=stats.rememberedPlayers;
    batch.missingRuntime=stats.missingRuntime;
    batch.filteredActors=stats.filteredActors;
    batch.invalidOwner=stats.invalidOwner;
    batch.missingShape=stats.missingShape;
    batch.invalidBounds=stats.invalidBounds;
    batch.captureStage=stats.stage ? stats.stage : "capture";
    batch.captureFailure=stats.failure ? stats.failure : (captured ? nullptr : captureError.c_str());
    batch.faultCode=stats.faultCode;
    batch.faultAddress=stats.faultAddress;
    for(std::size_t i=0;i<batch.count;++i) batch.samples[i]=publicSamples[i];
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
        if (!captured) {
            std::size_t copied=0;
            std::vector<bedrock::sdk::ActorSample> copies;
            std::vector<bedrock::sdk::ActorSample> handles;
            actorFacade.refreshTrackedPoses(local,handles,copies,copied);
            return;
        }
    } else if (loggedNativeFailure) {
        loggedNativeFailure=false;
        log("Actor capture recovered: pool=", batch.population, " copied=", batch.count);
    }
    {
        auto rows=actorFacade.playerNames(world);
        if(!rows.empty()) {
            std::array<hitbox::CopiedPlayerName,512> rosterCopies{};
            std::size_t count=0;
            for(const auto& row:rows) {
                if(count==rosterCopies.size()) break;
                auto& target=rosterCopies[count++];
                target={}; target.uuid={row.uuidLow,row.uuidHigh}; target.uniqueId=row.uniqueId;
                target.size=std::min(row.name.size(),target.bytes.size());
                std::memcpy(target.bytes.data(),row.name.data(),target.size);
            }
            playerNames.publish(batch.camera.epoch,rosterCopies.data(),count,std::chrono::minutes(config->nameCacheMinutes));
        }
    }
    // Native position capture is one game tick for every tracked valid handle.
    {
        std::size_t copied=0;
        std::vector<bedrock::sdk::ActorSample> handles(batch.samples.begin(),batch.samples.begin()+batch.count);
        std::vector<bedrock::sdk::ActorSample> copies;
        if(actorFacade.refreshTrackedPoses(local,handles,copies,copied)) {
            for(std::size_t i=0;i<std::min(copied,batch.count);++i) batch.samples[i]=copies[i];
        }
    }
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
void observeWorld(const bedrock::sdk::LevelTickEventValue& event) {
    if (!event.level) {
        if(currentWorld.exchange(nullptr,std::memory_order_acq_rel))
            worldEpoch.fetch_add(1,std::memory_order_acq_rel);
        return;
    }
    currentWorld.store(event.level, std::memory_order_release);
    try { refreshGameTickSnapshot(); }
    catch (...) { log("native capture skipped; retaining worker snapshots"); }
}

// Direct native render implementation is replaced by bedrock::sdk::WorldRender;
// HitBox keeps snapshot/cache/worker state and submits through drawLines/drawNameQuad.

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

    std::string error;
    bedrock::sdk::Profiles profiles;
    if(!profiles.open(bedrock::sdk::ProfileUpdatePolicy::RefreshIfNeeded,error)) {
        log("profile unavailable: ",error);
        status.store(Status::Failed,std::memory_order_release);
        return 1;
    }
    const auto profile=profiles.snapshot();
    bedrock::sdk::SymbolResolver resolver;
    if(!resolver.open(profile,error)) {
        log("profile resolver unavailable: ",error);
        status.store(Status::Failed,std::memory_order_release);
        return 1;
    }
    if(!actorFacade.configure(profile.json(),0,319,resolver,error)) {
        log("Actor facade configure failed: ",error);
        status.store(Status::Failed,std::memory_order_release);
        return 1;
    }
    log("native Actor registry source ready [all valid RuntimeID/AABB Actors including Players; ActorType + remembered RuntimeID player filter; no name filter; profile candidate]");

    if (!startProcessingWorkers()) { status.store(Status::Failed); return 1; }
    struct WorkerStop { ~WorkerStop() { stopProcessingWorkers(); } } workerStop;
    log(workerCount," processing workers + 1 log writer ready [RuntimeID partitions; 20Hz coalesced snapshots; packed text quads; copied scalars only]");
    gameHooks = std::make_unique<bedrock::sdk::Hooks>();
    nameTagSubscription=std::make_unique<bedrock::sdk::HookSubscription>(
        gameHooks->onActorNameTag(observeNameTag));
    gameSubscriptions.emplace_back(gameHooks->onGameplay([](const bedrock::sdk::GameplayEventValue& event) {
        if(event.clientInstance) hitboxClient.store(event.clientInstance,std::memory_order_release);
    }));
    gameSubscriptions.emplace_back(gameHooks->onLevelTick(observeWorld));
    gameSubscriptions.emplace_back(gameHooks->onWorldChange([](const bedrock::sdk::WorldChangeEventValue& event) {
        worldEpoch.fetch_add(1,std::memory_order_acq_rel);
        currentWorld.store(nullptr,std::memory_order_release);
        hitboxClient.store(event.clientInstance,std::memory_order_release);
    }));
    if(!gameHooks->install(resolver,profile.json(),error)) {
        log("SDK hook registration failed: ",error);
        gameHooks.reset();
        status.store(Status::Failed,std::memory_order_release);
        return 1;
    }
    log("NameTag configuration: show_name_tags=",std::atomic_load(&liveConfig)->showNameTags,
        " name_tag_scale=",std::atomic_load(&liveConfig)->nameTagScale);
    log("NameTag observation active [SDK Hooks::onActorNameTag; missing-name fallback; worker font geometry] diagnostic=",bedrock::sdk::hookRuntimeDiagnostic());
    bedrock::sdk::WorldRender worldRender;
    if(!worldRender.install(profile,error)) {
        log("world renderer install failed: ",error);
        gameHooks.reset();
        status.store(Status::Failed,std::memory_order_release);
        return 1;
    }
    auto worldRenderSubscription=worldRender.onPostRender([](bedrock::sdk::WorldRenderer& renderer) {
        if(stopRequested.load(std::memory_order_acquire)) return;
        const auto origin=renderer.origin();
        const auto epoch=worldEpoch.load(std::memory_order_acquire);
        const auto config=std::atomic_load_explicit(&liveConfig,std::memory_order_acquire);
        ++nameDiagnostics.renderCalls;
        ++nameDiagnostics.renderFrames;
        std::array<std::vector<bedrock::sdk::WorldLineSegment>,2> segments;
        for(std::size_t i=0;i<workerCount;++i) {
            const auto snapshot=std::atomic_load_explicit(&processingWorkers[i].snapshot,std::memory_order_acquire);
            if(!snapshot || snapshot->epoch!=epoch) continue;
            for(const auto& box:snapshot->boxes) {
                const auto cornersNow=hitbox::corners(interpolatedBounds(box,Clock::now()));
                constexpr unsigned edges[12][2]={{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
                auto& pass=segments[box.retained?1:0];
                for(const auto& edge:edges) pass.push_back({{cornersNow[edge[0]].x-origin.x,cornersNow[edge[0]].y-origin.y,cornersNow[edge[0]].z-origin.z},
                                                           {cornersNow[edge[1]].x-origin.x,cornersNow[edge[1]].y-origin.y,cornersNow[edge[1]].z-origin.z}});
            }
        }
        for(std::size_t pass=0;pass<2;++pass) if(!segments[pass].empty()) {
            const auto packed=pass ? config->retainedColor : config->liveColor;
            const auto rgba=hitbox::opaqueColor(packed);
            if(!renderer.drawLines(segments[pass],{rgba[0],rgba[1],rgba[2],rgba[3]},config->lineThickness))
                ++nameDiagnostics.renderFaults;
        }

        if(config->showNameTags) for(std::size_t i=0;i<workerCount;++i) {
            const auto snapshot=std::atomic_load_explicit(&processingWorkers[i].snapshot,std::memory_order_acquire);
            if(!snapshot || snapshot->epoch!=epoch || !snapshot->preparedNames) continue;
            const auto& prepared=*snapshot->preparedNames;
            for(const auto& label:prepared.labels) {
                if(label.boxIndex>=snapshot->boxes.size() || label.firstVertex>=prepared.vertices.size() ||
                   label.vertexCount>prepared.vertices.size()-label.firstVertex) { ++nameDiagnostics.renderNoMesh; continue; }
                const auto& box=snapshot->boxes[label.boxIndex];
                const auto packed=box.retained ? config->retainedColor : config->liveColor;
                const auto rgba=hitbox::opaqueColor(packed);
                std::size_t primitives=label.vertexCount/8;
                for(std::size_t p=0;p<primitives;++p) {
                    const auto first=label.firstVertex+p*8;
                    std::array<bedrock::sdk::WorldNameVertex,4> quad{};
                    quad[0].position=prepared.vertices[first]; quad[1].position=prepared.vertices[first+1];
                    quad[2].position=prepared.vertices[first+2]; quad[3].position=prepared.vertices[first+3];
                    if(renderer.drawNameQuad(quad,{rgba[0],rgba[1],rgba[2],rgba[3]}))
                        ++nameDiagnostics.renderSubmitted;
                    else ++nameDiagnostics.renderFaults;
                }
            }
        }
    });
    (void)worldRenderSubscription;
    log("SDK world rendering active [WorldRender + name-tag hooks; 1-tick target sweep]");
    log("ready ",profile.version()," [SDK native Actors]");
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
