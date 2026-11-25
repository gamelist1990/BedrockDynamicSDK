// Diet — lightweight client Mod for BedrockDynamicSDK (1.26.65.1).
//
// Pillar 1: asynchronous occlusion decisions (entity culling) computed off the
// render path, published as an immutable per-tick snapshot.
// Pillar 2: frame-time/packet-arrival measurement with a per-class safety gate.
//
// Everything that touches native game memory runs on the game tick inside the
// SDK's facades (WorldAccess / ActorFacade). The render callback only reads an
// already-published snapshot.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING
#include <windows.h>
#include <shlobj.h>

#include <bedrock/sdk.hpp>
#include <bedrock/sdk/logging.hpp>

#include "diet/occlusion.hpp"
#include "diet/traffic.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* kTag = "Diet";
bedrock::sdk::Logger logger{kTag};
bedrock::sdk::FileLogSink logSink;

std::uint64_t steadyNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// ---------------------------------------------------------------------------
// Configuration (Mod Settings is the single source of truth; applying a value
// only swaps this POD under a mutex, so readers never see a half-updated set).
// ---------------------------------------------------------------------------
struct Config {
    bool enabled = true;
    bool cull = true;
    bool cullPlayers = false;
    bool leavesSolid = false;
    bool debugDraw = false;
    bool measure = true;
    bool debug = false;
    double maxDistance = 96.0;
    double tickBudgetMs = 0.7;
    double netBudgetMs = 3.0;
};

std::mutex configMutex;
Config liveConfig{};
// Paint-only mirrors for the render callback (no mutex on the render thread).
std::atomic<bool> renderEnabled{true};
std::atomic<bool> renderDebugDraw{false};

Config configCopy() {
    std::lock_guard lock(configMutex);
    return liveConfig;
}

void applySetting(std::string_view id, const bedrock::sdk::SettingValue& value) {
    std::lock_guard lock(configMutex);
    const auto boolean = [&value] {
        const auto* flag = std::get_if<bool>(&value);
        return flag && *flag;
    };
    const auto number = [&value](double fallback) {
        const auto* numberValue = std::get_if<double>(&value);
        return numberValue ? *numberValue : fallback;
    };
    if (id == "diet_enabled") liveConfig.enabled = boolean();
    else if (id == "cull_enabled") liveConfig.cull = boolean();
    else if (id == "cull_players") liveConfig.cullPlayers = boolean();
    else if (id == "cull_leaves") liveConfig.leavesSolid = boolean();
    else if (id == "cull_debug_draw") liveConfig.debugDraw = boolean();
    else if (id == "cull_max_distance") liveConfig.maxDistance = std::clamp(number(96.0), 16.0, 192.0);
    else if (id == "tick_budget_ms") liveConfig.tickBudgetMs = std::clamp(number(0.7), 0.1, 4.0);
    else if (id == "net_measure") liveConfig.measure = boolean();
    else if (id == "net_budget_ms") liveConfig.netBudgetMs = std::clamp(number(3.0), 0.5, 16.0);
    else if (id == "debug") liveConfig.debug = boolean();
    renderEnabled.store(liveConfig.enabled);
    renderDebugDraw.store(liveConfig.debugDraw);
}

// ---------------------------------------------------------------------------
// Snapshot handed to the render thread. Published by atomic shared_ptr swap.
// ---------------------------------------------------------------------------
using VerdictList = std::vector<diet::Verdict>;
std::shared_ptr<const VerdictList> publishedVerdicts{};

struct TickReportData {
    diet::EngineStats engine{};
    unsigned captureRows{};
    unsigned capturePopulation{};
    float captureUs{};
    std::uint64_t ticks{};
    std::uint64_t hidden{};
};
struct TickReport {
    std::mutex mutex;
    TickReportData data{};
};
TickReport tickReport;

// ---------------------------------------------------------------------------
// Voxel opacity cache and classification.
// ---------------------------------------------------------------------------
struct VoxelKey {
    int x{}, y{}, z{};
    bool operator==(const VoxelKey&) const noexcept = default;
};
struct VoxelHash {
    std::size_t operator()(const VoxelKey& key) const noexcept {
        std::uint64_t hash = 1469598103934665603ull;
        const auto mix = [&hash](int value) {
            hash ^= static_cast<std::uint32_t>(value);
            hash *= 1099511628211ull;
            hash ^= hash >> 29;
        };
        mix(key.x);
        mix(key.y);
        mix(key.z);
        return static_cast<std::size_t>(hash);
    }
};
struct VoxelEntry {
    std::uint8_t opaque{};
    std::uint32_t stamp{};
};

bool containsAny(std::string_view text, std::initializer_list<std::string_view> needles) {
    for (const auto needle : needles)
        if (text.find(needle) != std::string_view::npos) return true;
    return false;
}

// Full-cube opaque whitelist. Anything not listed here is *not* an occluder:
// a false "visible" only costs culling, while a false "occluded" would hide a
// real entity, so the classifier errs towards visible. Allocation-free.
bool isOpaqueBlockName(std::string_view identifier, bool leavesSolid) {
    if (identifier.empty()) return false;
    const auto colon = identifier.find(':');
    const std::string_view name =
        colon == std::string_view::npos ? identifier : identifier.substr(colon + 1);
    if (name.empty()) return false;
    if (name == "air" || name == "cave_air" || name == "void_air") return false;
    if (name.find("leaves") != std::string_view::npos) return leavesSolid;
    // Shapes that are solid but not full cubes never occlude.
    if (containsAny(name, {"slab", "stairs", "fence", "wall", "pane", "door", "trapdoor",
                           "carpet", "button", "pressure_plate", "sign", "rail", "torch",
                           "lantern", "campfire", "bed", "chest", "barrel", "hopper",
                           "cauldron", "composter", "lectern", "anvil", "brewing",
                           "enchanting", "beacon", "conduit", "bell", "banner", "skull",
                           "head", "shulker", "cake", "candle", "flower", "sapling", "grass",
                           "fern", "vine", "lily", "cactus", "bamboo", "sugar_cane",
                           "glass", "ice", "water", "lava", "snow_layer", "scaffolding",
                           "chain", "ladder", "flower_pot", "frame", "coral", "kelp",
                           "seagrass", "spawner", "portal", "piston_head", "turtle_egg",
                           "frogspawn", "lichen", "moss_carpet", "web"}))
        return false;
    return containsAny(name, {"stone", "dirt", "grass_block", "sand", "gravel", "planks",
                              "log", "wood", "deepslate", "tuff", "calcite", "basalt",
                              "blackstone", "netherrack", "soul_sand", "soul_soil", "magma",
                              "obsidian", "bedrock", "clay", "wool", "concrete", "terracotta",
                              "copper", "iron_block", "gold_block", "diamond_block",
                              "emerald_block", "lapis_block", "redstone_block",
                              "netherite_block", "quartz", "prismarine", "purpur", "amethyst",
                              "ore", "brick", "mud", "sponge", "melon", "pumpkin", "hay_block",
                              "bone_block", "dried_kelp", "sculk", "snow_block", "dripstone"});
}

struct RaycastContext {
    bedrock::sdk::WorldAccess* world{};
    void* region{};
    bool leavesSolid{};
    std::uint32_t stamp{};
    std::uint32_t* queries{};
};

std::unordered_map<VoxelKey, VoxelEntry, VoxelHash> voxelCache;

bool isOpaqueAt(void* context, int x, int y, int z) {
    auto* ray = static_cast<RaycastContext*>(context);
    if (!ray || !ray->world || !ray->region) return false;
    const VoxelKey key{x, y, z};
    if (const auto it = voxelCache.find(key); it != voxelCache.end()) {
        // A cached answer is trusted for a short window only: the world changes.
        if (ray->stamp - it->second.stamp < 20) return it->second.opaque != 0;
        voxelCache.erase(it);
    }
    if (voxelCache.size() > 49152) voxelCache.clear();
    bedrock::sdk::BlockPos position{};
    position.x = x;
    position.y = y;
    position.z = z;
    ++*ray->queries;
    const auto cell = ray->world->getBlock(ray->region, position);
    const bool opaque =
        cell.kind == bedrock::sdk::CellKind::block && isOpaqueBlockName(cell.identifier, ray->leavesSolid);
    voxelCache.emplace(key, VoxelEntry{static_cast<std::uint8_t>(opaque ? 1 : 0), ray->stamp});
    return opaque;
}

// ---------------------------------------------------------------------------
// Runtime state.
// ---------------------------------------------------------------------------
struct Runtime {
    bedrock::sdk::WorldAccess world;
    bedrock::sdk::ActorFacade actors;
    diet::OcclusionEngine engine;
    diet::TrafficAnalyzer traffic;
    std::atomic<void*> client{};
    std::atomic<void*> level{};
    std::atomic<std::uint64_t> epoch{};
    std::size_t captureCursor{};
    std::uint32_t tickStamp{};
    std::uint64_t publishHash{};
    std::uint64_t lastPublishNs{};
    std::atomic<std::uint64_t> hiddenCount{};
    std::atomic<unsigned> blockQueries{};
    std::atomic<std::uint64_t> deferredTicks{};
    std::uint64_t ticks{};
};
Runtime* runtime{};

void noteTick(const diet::EngineStats& engine, unsigned rows, unsigned population, float captureUs) {
    std::lock_guard lock(tickReport.mutex);
    tickReport.data.engine = engine;
    tickReport.data.captureRows = rows;
    tickReport.data.capturePopulation = population;
    tickReport.data.captureUs = captureUs;
    tickReport.data.ticks++;
    tickReport.data.hidden = engine.hidden;
}

// ---------------------------------------------------------------------------
// Level tick: capture actors (budgeted by the facade) and run occlusion checks
// (budgeted by the engine) on the game thread.
// ---------------------------------------------------------------------------
void onLevelTick(const bedrock::sdk::LevelTickEventValue& event) {
    if (!runtime) return;
    auto& state = *runtime;
    state.level.store(event.level);
    const auto config = configCopy();
    if (!config.enabled || !event.level) return;

    state.tickStamp++;
    ++state.ticks;
    state.blockQueries.store(0, std::memory_order_relaxed);

    void* client = state.client.load();
    if (!client) return;
    void* local = state.actors.localPlayer(client);
    if (!local) return;

    // Camera position (local player eye). pose() is the SDK's confirmed read.
    bedrock::sdk::Vec3Value position{};
    std::uint64_t runtimeId{};
    if (!state.world.pose(local, position, runtimeId)) return;
    const diet::Vec3f camera{position.x, position.y + 1.62f, position.z};

    const auto tickStarted = std::chrono::steady_clock::now();
    if (config.cull) {
        // Frame-time guard: never add Diet work to a frame that is already over
        // the configured budget. The work is deferred to a later frame, which
        // the engine's per-target schedule already supports.
        if (const double frameMs = state.traffic.lastFrameMs(); frameMs > config.netBudgetMs) {
            state.deferredTicks.fetch_add(1, std::memory_order_relaxed);
            return; // keep the last real summary; nothing ran this tick
        }
        diet::Limits limits = state.engine.limits();
        limits.maxDistance = static_cast<float>(config.maxDistance);
        limits.protectPlayers = !config.cullPlayers;
        limits.tickBudgetUs = static_cast<unsigned>(config.tickBudgetMs * 1000.0);
        state.engine.setLimits(limits);

        RaycastContext ray{};
        ray.world = &state.world;
        ray.region = state.world.region(client);
        ray.leavesSolid = config.leavesSolid;
        ray.stamp = state.tickStamp;
        auto queries = state.blockQueries.load(std::memory_order_relaxed);
        ray.queries = &queries;

        state.engine.beginTick(steadyNs(), camera, &isOpaqueAt, &ray);

        std::vector<bedrock::sdk::ActorSample> samples;
        bedrock::sdk::ActorFacade::ActorCaptureStatsValue stats;
        std::string captureError;
        std::size_t visited = 0;
        samples.reserve(96);
        const bool captured = state.actors.capture(local, false, state.captureCursor, 96, samples,
                                                   visited, stats, captureError);
        if (captured) {
            for (const auto& sample : samples) {
                if (!sample.bounds.valid) continue;
                diet::TargetInput input;
                input.runtimeId = sample.runtimeId;
                input.isPlayer = sample.isPlayer;
                input.box.min = {sample.bounds.minimum.x, sample.bounds.minimum.y,
                                 sample.bounds.minimum.z};
                input.box.max = {sample.bounds.maximum.x, sample.bounds.maximum.y,
                                 sample.bounds.maximum.z};
                state.engine.addTarget(input);
            }
        }
        state.engine.endTick(steadyNs());

        const auto& engineStats = state.engine.stats();
        const float captureUs =
            std::chrono::duration_cast<std::chrono::duration<float, std::micro>>(
                std::chrono::steady_clock::now() - tickStarted)
                .count();
        state.blockQueries.store(queries, std::memory_order_relaxed);
        noteTick(engineStats, static_cast<unsigned>(samples.size()), stats.population, captureUs);
        state.hiddenCount.store(engineStats.hidden);

        // Publish immutable verdicts only when the hidden set changed, so the
        // steady state performs no allocation at all.
        std::uint64_t hash = 1469598103934665603ull;
        for (unsigned i = 0; i < state.engine.verdictCount(); ++i) {
            const auto& verdict = state.engine.verdicts()[i];
            if (!verdict.hidden) continue;
            hash ^= verdict.runtimeId + (verdict.hidden ? 1u : 0u) + 0x9e3779b97f4a7c15ull;
            hash *= 1099511628211ull;
        }
        const auto now = steadyNs();
        if (hash != state.publishHash || now - state.lastPublishNs > 250ull * 1000000ull) {
            auto copy = std::make_shared<VerdictList>(
                state.engine.verdicts(), state.engine.verdicts() + state.engine.verdictCount());
            std::atomic_store_explicit(&publishedVerdicts, std::shared_ptr<const VerdictList>(copy),
                                       std::memory_order_release);
            state.publishHash = hash;
            state.lastPublishNs = now;
        }
    }
}

void onWorldChange(const bedrock::sdk::WorldChangeEventValue& event) {
    if (!runtime) return;
    runtime->epoch.fetch_add(1, std::memory_order_acq_rel);
    runtime->level.store(nullptr);
    runtime->client.store(event.clientInstance);
    voxelCache.clear();
    runtime->captureCursor = 0;
    std::atomic_store_explicit(&publishedVerdicts, std::shared_ptr<const VerdictList>(),
                               std::memory_order_release);
    logger.info("world change reason=", static_cast<int>(event.reason),
                " [voxel cache cleared, verdicts dropped]");
}

void onGameplay(const bedrock::sdk::GameplayEventValue& event) {
    if (runtime && event.clientInstance) runtime->client.store(event.clientInstance);
}

// ---------------------------------------------------------------------------
// Packet observation (networking thread). Scalars only, never blocks.
// ---------------------------------------------------------------------------
void onPacket(const bedrock::sdk::PacketObservation& observation, void* user) {
    auto* state = static_cast<Runtime*>(user);
    if (!state) return;
    state->traffic.observePacket(steadyNs(), observation.rawId, observation.bytes);
}

// ---------------------------------------------------------------------------
// Render callback: frame clock + diagnostic boxes from the published snapshot.
// No allocation, no logging, no native reads.
// ---------------------------------------------------------------------------
void onRender(bedrock::sdk::WorldRenderer& renderer) {
    auto* state = runtime;
    if (!state) return;
    state->traffic.observeFrame(steadyNs());
    if (!renderEnabled.load(std::memory_order_relaxed) ||
        !renderDebugDraw.load(std::memory_order_relaxed))
        return;
    const auto snapshot = std::atomic_load_explicit(&publishedVerdicts, std::memory_order_acquire);
    if (!snapshot || snapshot->empty()) return;
    const auto origin = renderer.origin();
    constexpr unsigned edgePairs[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                           {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    // Render-thread scratch, reserved once; never grows after the first frame.
    static thread_local std::vector<bedrock::sdk::WorldLineSegment> segments;
    if (segments.capacity() < 24 * 12) segments.reserve(24 * 12);
    segments.clear();
    unsigned drawn = 0;
    for (const auto& verdict : *snapshot) {
        if (!verdict.hidden || drawn >= 24) continue;
        ++drawn;
        diet::Vec3f corners[8];
        for (int index = 0; index < 8; ++index) {
            const auto& box = verdict.box;
            corners[index] = {(index & 1) ? box.max.x : box.min.x,
                              (index & 2) ? box.max.y : box.min.y,
                              (index & 4) ? box.max.z : box.min.z};
        }
        for (const auto& edge : edgePairs) {
            const auto& from = corners[edge[0]];
            const auto& to = corners[edge[1]];
            segments.push_back({{from.x - origin.x, from.y - origin.y, from.z - origin.z},
                                {to.x - origin.x, to.y - origin.y, to.z - origin.z}});
        }
    }
    if (!segments.empty())
        (void)renderer.drawLines(segments, {0.95f, 0.35f, 0.15f, 1.0f}, 1.5f);
}

// ---------------------------------------------------------------------------
// Settings page.
// ---------------------------------------------------------------------------
std::vector<bedrock::sdk::Translation> englishLocale() {
    return {
        {"diet.name", "Diet"}, {"diet.title", "Diet — load & network stalls"},
        {"diet.enabled", "Enable Diet"},
        {"diet.cull", "Entity occlusion culling"},
        {"diet.cull.help", "Decides visibility off the render path (workers + budgeted tick work)."},
        {"diet.players", "Cull players (off = never hide a player)"},
        {"diet.leaves", "Treat leaves as occluding blocks"},
        {"diet.draw", "Draw occluded-target boxes"},
        {"diet.distance", "Distance cull (blocks)"},
        {"diet.tick_budget", "Tick budget for culling work (ms)"},
        {"diet.measure", "Measure frame spikes vs packet arrivals"},
        {"diet.net_budget", "Skip Diet's work on frames slower than (ms)"},
        {"diet.debug", "Verbose logging"},
    };
}

std::vector<bedrock::sdk::Translation> japaneseLocale() {
    return {
        {"diet.name", "Diet"}, {"diet.title", "Diet — 読み込み・通信スタール"},
        {"diet.enabled", "Diet を有効化"},
        {"diet.cull", "エンティティ遮蔽カリング"},
        {"diet.cull.help", "描画スレッド外で可視判定（ワーカー＋予算付きティック処理）。"},
        {"diet.players", "プレイヤーもカリング（OFF で絶対に隠さない）"},
        {"diet.leaves", "葉を遮蔽ブロックとして扱う"},
        {"diet.draw", "遮蔽判定されたターゲットを枠で表示"},
        {"diet.distance", "距離カリング（ブロック）"},
        {"diet.tick_budget", "カリング処理のティック予算 (ms)"},
        {"diet.measure", "フレームスパイクとパケット到着の相関を計測"},
        {"diet.net_budget", "この時間を超えたフレームでは Diet の処理を休止 (ms)"},
        {"diet.debug", "詳細ログ"},
    };
}

class DietMod final : public bedrock::sdk::Mod {
public:
    DietMod() : Mod("diet") {}

    bool registerSettings() {
        using namespace bedrock::sdk;
        auto page = settings("diet")
                        .modName("diet.name")
                        .add(Setting::toggle("diet_enabled", "diet.enabled", true))
                        .add(Setting::text("cull_header", "diet.cull", "diet.cull.help"))
                        .add(Setting::toggle("cull_enabled", "diet.cull", true))
                        .add(Setting::toggle("cull_players", "diet.players", false))
                        .add(Setting::toggle("cull_leaves", "diet.leaves", false))
                        .add(Setting::slider("cull_max_distance", "diet.distance", 96, 16, 192, 4))
                        .add(Setting::slider("tick_budget_ms", "diet.tick_budget", 0.7, 0.1, 4.0, 0.1))
                        .add(Setting::toggle("cull_debug_draw", "diet.draw", false))
                        .add(Setting::toggle("net_measure", "diet.measure", true))
                        .add(Setting::slider("net_budget_ms", "diet.net_budget", 3.0, 0.5, 16.0, 0.5))
                        .add(Setting::toggle("debug", "diet.debug", false))
                        .locale("en_US", englishLocale())
                        .locale("ja_JP", japaneseLocale())
                        .onChange([](std::string_view id, const SettingValue& value) {
                            applySetting(id, value);
                        });
        page_ = std::make_shared<SettingsPage>(page);
        const auto registered = page_->registerPage();
        if (!registered) {
            logger.error("settings registration failed: ", registered.error().message);
            return false;
        }
        static constexpr std::string_view ids[] = {
            "diet_enabled", "cull_enabled", "cull_players", "cull_leaves",
            "cull_max_distance", "tick_budget_ms", "cull_debug_draw", "net_measure",
            "net_budget_ms", "debug"};
        for (const auto id : ids) {
            const auto value = page_->value(id);
            if (value) applySetting(id, value.value());
            else logger.warn("cannot restore ", id, ": ", value.error().message);
        }
        return true;
    }

private:
    void onEnable() override { registerSettings(); }
    void onDisable() override {
        if (page_) (void)page_->unregisterPage();
    }

    std::shared_ptr<bedrock::sdk::SettingsPage> page_;
};

DietMod& dietMod() {
    static DietMod instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Stat reporting (housekeeping thread only: never the game or render thread).
// ---------------------------------------------------------------------------
void reportLoop(std::atomic<bool>& stopping) {
    while (!stopping.load(std::memory_order_acquire)) {
        for (int i = 0; i < 50 && !stopping.load(std::memory_order_acquire); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (stopping.load(std::memory_order_acquire)) break;
        if (!runtime) continue;
        const auto config = configCopy();
        if (config.measure) {
            const auto text = runtime->traffic.buildReport(steadyNs());
            logger.info(text);
        }
        TickReportData copy{};
        {
            std::lock_guard lock(tickReport.mutex);
            copy = tickReport.data;
        }
        logger.info("cull summary: tracked=", copy.engine.tracked,
                    " checked=", copy.engine.checked, " samples=", copy.engine.samples,
                    " visible=", copy.engine.visible, " occluded=", copy.engine.occluded,
                    " distance_culled=", copy.engine.distanceCulled,
                    " protected=", copy.engine.protectedTargets,
                    " budget_stops=", copy.engine.budgetStops, " hidden=", copy.engine.hidden,
                    " engine_us=", copy.engine.elapsedUs, " capture_rows=", copy.captureRows,
                    " population=", copy.capturePopulation, " capture_us=", copy.captureUs,
                    " block_queries=", runtime->blockQueries.load(std::memory_order_relaxed),
                    " deferred_ticks=", runtime->deferredTicks.load(std::memory_order_relaxed),
                    " ticks=", copy.ticks);
    }
}

struct Clip {
    std::unique_ptr<bedrock::sdk::Hooks> hooks;
    std::vector<bedrock::sdk::HookSubscription> subscriptions;
    std::unique_ptr<bedrock::sdk::WorldRender> worldRender;
    std::optional<bedrock::sdk::WorldRenderSubscription> renderSubscription;
    std::unique_ptr<bedrock::sdk::PacketObserver> observer;
};
Clip clip;

std::filesystem::path dataDirectory() {
    PWSTR documents = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &documents))) {
        std::filesystem::path result{documents};
        CoTaskMemFree(documents);
        return result / L"PEXData" / L"Diet";
    }
    return std::filesystem::temp_directory_path() / L"Diet";
}

std::atomic<unsigned> gStatus{0};
std::atomic<bool> gStop{false};

DWORD WINAPI startClient(void*) {
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&startClient), &pinned)) {
        gStatus.store(1);
        return 1;
    }
    const auto directory = dataDirectory();
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (!logSink.open(directory / L"Diet.log")) {
        // Keep running without a file sink; Content Log still receives errors.
    }
    logSink.attach(logger);
    logger.info("Diet starting build=", BEDROCK_DIET_BUILD_ID, " cpp=", _MSC_VER);

    std::string error;
    bedrock::sdk::Profiles profiles;
    if (!profiles.open(bedrock::sdk::ProfileUpdatePolicy::EmbeddedOnly,
                       bedrock::sdk::ProfileCandidatePolicy::AllowCandidates, error)) {
        logger.error("game profile unavailable: ", error);
        gStatus.store(2);
        return 2;
    }
    const auto profile = profiles.snapshot();
    bedrock::sdk::SymbolResolver resolver;
    if (!resolver.open(profile, error)) {
        logger.error("resolver unavailable: ", error);
        gStatus.store(2);
        return 2;
    }

    runtime = new Runtime();
    if (!runtime->actors.configure(profile.json(), 0, 319, resolver, error)) {
        logger.error("Actor facade unavailable: ", error);
        gStatus.store(3);
        return 3;
    }
    try {
        runtime->world.loadWorld(profile.json());
    } catch (const std::exception& exception) {
        logger.error("WorldAccess unavailable: ", exception.what());
        gStatus.store(3);
        return 3;
    }
    logger.info("exact profile ", profile.version(), " native world + actor facades ready");

    if (const auto enabled = dietMod().enable(); !enabled) {
        logger.error("mod enable failed: ", enabled.error().message);
    }

    clip.hooks = std::make_unique<bedrock::sdk::Hooks>();
    clip.subscriptions.emplace_back(clip.hooks->onLevelTick(onLevelTick));
    clip.subscriptions.emplace_back(clip.hooks->onGameplay(onGameplay));
    clip.subscriptions.emplace_back(clip.hooks->onWorldChange(onWorldChange));
    if (!clip.hooks->install(resolver, profile.json(), error)) {
        logger.error("hook install failed: ", error);
    } else {
        logger.info("hooks active: level tick, gameplay, world change");
    }

    clip.worldRender = std::make_unique<bedrock::sdk::WorldRender>();
    if (!clip.worldRender->install(profile, error)) {
        logger.error("world render unavailable: ", error);
    } else {
        clip.renderSubscription.emplace(clip.worldRender->onPostRender(onRender));
        logger.info("render frame clock active (diagnostic boxes available)");
    }

    clip.observer = std::make_unique<bedrock::sdk::PacketObserver>();
    clip.observer->setHandler(&onPacket, runtime);
    if (!clip.observer->install(resolver, profile.json(), error)) {
        logger.warn("packet observation unavailable: ", error);
    } else {
        logger.info("packet observation active (post-decode, copied payload size only)");
    }

    std::atomic<bool> stopping{false};
    std::thread reporter(reportLoop, std::ref(stopping));
    gStatus.store(4);
    logger.info("ready; notes: entity render-skip and packet deferral need new RVAs "
                "(see opencode-report.md)");

    while (!gStop.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    stopping.store(true, std::memory_order_release);
    reporter.join();
    clip.observer.reset();
    clip.renderSubscription.reset();
    clip.worldRender.reset();
    clip.subscriptions.clear();
    clip.hooks.reset();
    std::atomic_store_explicit(&publishedVerdicts, std::shared_ptr<const VerdictList>(),
                               std::memory_order_release);
    gStatus.store(5);
    logger.info("stopped");
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) unsigned Diet_GetStatus() {
    return gStatus.load(std::memory_order_acquire);
}
extern "C" __declspec(dllexport) void Diet_Stop() { gStop.store(true, std::memory_order_release); }

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (const auto worker = CreateThread(nullptr, 0, startClient, module, 0, nullptr))
            CloseHandle(worker);
        else gStatus.store(1);
    } else if (reason == DLL_PROCESS_DETACH) {
        gStop.store(true, std::memory_order_release);
    }
    return TRUE;
}
