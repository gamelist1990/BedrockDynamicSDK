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
#include "diet/debug_screen.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
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

std::string hexPointer(std::uintptr_t value) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
    return buffer;
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
    bool skipDraw = true;
    bool renderProbe = false;
    bool measure = true;
    bool defer = true;
    bool debug = false;
    bool f3 = false;
    double maxDistance = 96.0;
    double tickBudgetMs = 0.7;
    double netBudgetMs = 3.0;
    unsigned deferBudget = 8;
};

std::mutex configMutex;
Config liveConfig{};
// Paint-only mirrors for the render callback (no mutex on the render thread).
std::atomic<bool> renderEnabled{true};
std::atomic<bool> renderDebugDraw{false};
// Harness marker (Diet.probe): observe the packet handlers without deferring,
// which is the logging-only stage of the BOL proof.
std::atomic<bool> deferProbeOnly{false};

Config configCopy() {
    std::lock_guard lock(configMutex);
    return liveConfig;
}

// Defined after Runtime: the settings callback may arrive before the screen
// exists, so this is a no-op until then.
void applyF3Visibility(bool visible);

// Long-lived SDK clips. Declared here because the tick and world-change
// callbacks (defined below the config block) use the packet-deferral clip.
struct Clip {
    std::unique_ptr<bedrock::sdk::Hooks> hooks;
    std::vector<bedrock::sdk::HookSubscription> subscriptions;
    std::unique_ptr<bedrock::sdk::WorldRender> worldRender;
    std::optional<bedrock::sdk::WorldRenderSubscription> renderSubscription;
    std::unique_ptr<bedrock::sdk::PacketObserver> observer;
    std::unique_ptr<bedrock::sdk::ActorRenderFilter> actorFilter;
    std::unique_ptr<bedrock::sdk::PacketDeferral> packetDeferral;
};
Clip clip;

// Pushes the kill switch to the installed adapter (no-op before install).
void syncDeferral() {
    const auto config = configCopy();
    if (clip.packetDeferral && clip.packetDeferral->ready())
        clip.packetDeferral->setEnabled(config.enabled && config.defer &&
                                        !deferProbeOnly.load(std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------
// Render skip. The render-thread predicate may only compare pointers, so the
// tick thread publishes the native Actor pointers it currently judges hidden
// into a fixed array; `hiddenPointerCount` is the release/acquire fence.
// ---------------------------------------------------------------------------
constexpr unsigned kMaxPublishedPointers = 64;
constexpr unsigned kMaxTrackedPointers = 16;
std::atomic<unsigned> hiddenPointerCount{0};
std::atomic<std::uintptr_t> hiddenPointers[kMaxPublishedPointers];
std::atomic<unsigned> trackedPointerCount{0};
std::atomic<std::uintptr_t> trackedPointers[kMaxTrackedPointers];
std::atomic<bool> skipDrawEnabled{true};
std::atomic<std::uint64_t> skippedDraws{0};
// Cumulative held-packet count (published to F3; the window count is logged).
std::atomic<std::uint64_t> deferredTotal{0};

// Runtime id -> native Actor pointer, learned from the actor capture. Only the
// tick thread touches this map.
std::unordered_map<std::uint64_t, std::uintptr_t> actorPointers;

void clearPublishedRenderState() {
    hiddenPointerCount.store(0, std::memory_order_release);
    trackedPointerCount.store(0, std::memory_order_release);
    actorPointers.clear();
}

// Render-thread policy: true excludes the object from the extraction pass.
// Runs once per object of the watched passes, so it is a bounded pointer scan
// with no allocation, no locks and no native reads.
bool shouldExcludeRenderObject(void* object) noexcept {
    if (!skipDrawEnabled.load(std::memory_order_relaxed) || !object) return false;
    const auto value = reinterpret_cast<std::uintptr_t>(object);
    const unsigned count = hiddenPointerCount.load(std::memory_order_acquire);
    for (unsigned index = 0; index < count; ++index)
        if (hiddenPointers[index].load(std::memory_order_relaxed) == value) return true;
    return false;
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
    else if (id == "cull_skip_draw") liveConfig.skipDraw = boolean();
    else if (id == "cull_probe") liveConfig.renderProbe = boolean();
    else if (id == "cull_max_distance") liveConfig.maxDistance = std::clamp(number(96.0), 16.0, 192.0);
    else if (id == "tick_budget_ms") liveConfig.tickBudgetMs = std::clamp(number(0.7), 0.1, 4.0);
    else if (id == "net_measure") liveConfig.measure = boolean();
    else if (id == "net_budget_ms") liveConfig.netBudgetMs = std::clamp(number(3.0), 0.5, 16.0);
    else if (id == "net_defer") liveConfig.defer = boolean();
    else if (id == "net_defer_budget")
        liveConfig.deferBudget =
            static_cast<unsigned>(std::clamp(number(8.0), 1.0, 64.0));
    else if (id == "f3_enabled") liveConfig.f3 = boolean();
    else if (id == "debug") liveConfig.debug = boolean();
    renderEnabled.store(liveConfig.enabled);
    renderDebugDraw.store(liveConfig.debugDraw);
    skipDrawEnabled.store(liveConfig.enabled && liveConfig.cull && liveConfig.skipDraw);
    syncDeferral();
    if (!liveConfig.enabled || !liveConfig.cull || !liveConfig.skipDraw)
        hiddenPointerCount.store(0, std::memory_order_release);
    applyF3Visibility(liveConfig.f3);
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
    // F3 pose mirror (float atomics: the render/housekeeping threads read
    // these without touching native state; only the tick writes them).
    std::atomic<float> poseX{}, poseY{}, poseZ{}, poseYaw{}, posePitch{};
    std::atomic<bool> poseValid{};
    std::atomic<bool> inWorld{};
    // Packet-id diagnostics: the F3 screen and the traffic report need the raw
    // ids the observer actually sees, so a mismatch with the classifier table
    // is visible instead of silently classifying everything as unknown.
    std::atomic<int> lastRawId{-1};
    std::atomic<int> rawIdMin{2147483647};
    std::atomic<int> rawIdMax{-2147483647 - 1};
    diet::DebugScreen debugScreen;
};
Runtime* runtime{};

void applyF3Visibility(bool visible) {
    if (runtime) runtime->debugScreen.setVisible(visible);
}

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

    // Panel 2: hand the held packets back under our own per-tick budget. The
    // handler only enqueues level work, so a handful of replays per tick keeps
    // the burst spread across frames instead of landing in one. This runs even
    // when the kill switch is off so that toggling it drains what is held.
    if (clip.packetDeferral && clip.packetDeferral->ready())
        (void)clip.packetDeferral->pump(config.deferBudget);

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

    // F3 pose mirror: feet position and rotation, published as plain floats so
    // the screen never reads native memory off the tick thread.
    state.poseX.store(position.x, std::memory_order_relaxed);
    state.poseY.store(position.y, std::memory_order_relaxed);
    state.poseZ.store(position.z, std::memory_order_relaxed);
    if (const auto layout = state.world.actors(); layout.actorRotationComponentHash) {
        if (void* rotation = state.world.component(local, layout.actorRotationComponentHash)) {
            float pitch = 0.0f;
            float yaw = 0.0f;
            if (state.world.readField(rotation, layout.rotationPitchOffset, pitch) &&
                state.world.readField(rotation, layout.rotationYawOffset, yaw)) {
                state.posePitch.store(pitch, std::memory_order_relaxed);
                state.poseYaw.store(yaw, std::memory_order_relaxed);
                state.poseValid.store(true, std::memory_order_relaxed);
            }
        }
    }

    const auto tickStarted = std::chrono::steady_clock::now();
    if (config.cull) {
        // Frame-time guard: never add Diet work to a frame that is already
        // over the configured budget. The work is deferred to a later frame,
        // which the engine's per-target schedule already supports. The
        // comparison is against the slower of the configured threshold and a
        // multiple of this machine's smoothed frame time, so a uniformly slow
        // machine (software rendering, a 20 fps laptop) is not permanently
        // classified as "stalling" and starved of culling.
        const double frameMs = state.traffic.lastFrameMs();
        const double baselineMs = state.traffic.smoothedFrameMs();
        const double guardMs = std::max(config.netBudgetMs, 2.5 * baselineMs);
        if (frameMs > guardMs) {
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
                if (sample.actorKey) {
                    actorPointers[sample.runtimeId] = sample.actorKey;
                }
            }
            if (actorPointers.size() > 8192) actorPointers.clear();
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

        // Publish the native pointers of the targets judged hidden right now,
        // plus a small sample of tracked pointers for the diagnostic probe.
        // A target without a known pointer is skipped, never guessed.
        unsigned published = 0;
        for (unsigned i = 0; i < state.engine.verdictCount() && published < kMaxPublishedPointers;
             ++i) {
            const auto& verdict = state.engine.verdicts()[i];
            if (!verdict.hidden || verdict.isPlayer) continue;
            const auto found = actorPointers.find(verdict.runtimeId);
            if (found == actorPointers.end() || !found->second) continue;
            hiddenPointers[published++].store(found->second, std::memory_order_relaxed);
        }
        hiddenPointerCount.store(published, std::memory_order_release);
        unsigned tracked = 0;
        for (const auto& sample : samples) {
            if (tracked >= kMaxTrackedPointers) break;
            if (!sample.actorKey) continue;
            trackedPointers[tracked++].store(sample.actorKey, std::memory_order_relaxed);
        }
        trackedPointerCount.store(tracked, std::memory_order_release);
    }
}

void onWorldChange(const bedrock::sdk::WorldChangeEventValue& event) {
    if (!runtime) return;
    runtime->epoch.fetch_add(1, std::memory_order_acq_rel);
    runtime->level.store(nullptr);
    runtime->client.store(event.clientInstance);
    runtime->poseValid.store(false, std::memory_order_relaxed);
    runtime->inWorld.store(false, std::memory_order_relaxed);
    runtime->debugScreen.setWorldActive(false);
    voxelCache.clear();
    runtime->captureCursor = 0;
    clearPublishedRenderState();
    // Held block updates belong to the level we are leaving; applying them to
    // the next one would be wrong, so they are dropped (the client re-syncs
    // chunks on entry anyway).
    unsigned droppedPackets = 0;
    if (clip.packetDeferral) droppedPackets = clip.packetDeferral->clear();
    std::atomic_store_explicit(&publishedVerdicts, std::shared_ptr<const VerdictList>(),
                               std::memory_order_release);
    logger.info("world change reason=", static_cast<int>(event.reason),
                " [voxel cache cleared, verdicts dropped, held packets dropped=",
                droppedPackets, "]");
}

void onGameplay(const bedrock::sdk::GameplayEventValue& event) {
    if (!runtime) return;
    if (event.clientInstance) runtime->client.store(event.clientInstance);
    // Java's F3 screen exists only inside a world; the same gate here keeps it
    // off the main menu and out of the pause screen of the title.
    runtime->inWorld.store(event.active, std::memory_order_relaxed);
    runtime->debugScreen.setWorldActive(event.active);
}

// F3 toggles the debug screen. Session-only (not persisted); key repeats are
// filtered so holding the key does not flicker the screen.
std::atomic<unsigned> keyEvents{};
std::atomic<int> lastKey{-1};
std::atomic<bool> f3Latch{false};

void toggleDebugScreen() {
    if (!runtime) return;
    runtime->debugScreen.toggle();
    logger.info("F3 debug screen ", runtime->debugScreen.visible() ? "on" : "off");
}

void onKey(bedrock::sdk::KeyEventValue& event) {
    keyEvents.fetch_add(1, std::memory_order_relaxed);
    lastKey.store(event.key, std::memory_order_relaxed);
    if (event.key != VK_F3) return;
    if (!event.pressed) {
        f3Latch.store(false, std::memory_order_relaxed);
        return;
    }
    if (f3Latch.exchange(true, std::memory_order_relaxed)) return;
    toggleDebugScreen();
}

void onNativeKey(const bedrock::sdk::NativeKeyEventValue& event) {
    keyEvents.fetch_add(1, std::memory_order_relaxed);
    lastKey.store(static_cast<int>(event.inputKey), std::memory_order_relaxed);
    if (event.inputKey != VK_F3) return;
    if (!event.pressed) {
        f3Latch.store(false, std::memory_order_relaxed);
        return;
    }
    if (f3Latch.exchange(true, std::memory_order_relaxed)) return;
    toggleDebugScreen();
}

// ---------------------------------------------------------------------------
// Packet observation (networking thread). Scalars only, never blocks.
// ---------------------------------------------------------------------------
void onPacket(const bedrock::sdk::PacketObservation& observation, void* user) {
    auto* state = static_cast<Runtime*>(user);
    if (!state) return;
    state->traffic.observePacket(steadyNs(), observation.rawId, observation.bytes);
    const int id = observation.rawId;
    state->lastRawId.store(id, std::memory_order_relaxed);
    int current = state->rawIdMin.load(std::memory_order_relaxed);
    while (id < current &&
           !state->rawIdMin.compare_exchange_weak(current, id, std::memory_order_relaxed)) {
    }
    current = state->rawIdMax.load(std::memory_order_relaxed);
    while (id > current &&
           !state->rawIdMax.compare_exchange_weak(current, id, std::memory_order_relaxed)) {
    }
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
        {"diet.skip", "Skip draws of hidden targets"},
        {"diet.probe", "Render probe (diagnostic, no exclusion)"},
        {"diet.distance", "Distance cull (blocks)"},
        {"diet.tick_budget", "Tick budget for culling work (ms)"},
        {"diet.measure", "Measure frame spikes vs packet arrivals"},
        {"diet.net_budget", "Stand down on frames slower than (ms) or 2.5x the recent average"},
        {"diet.defer", "Spread delay-tolerant packet handling over ticks (block updates only)"},
        {"diet.defer_budget", "Packets handed back per tick"},
        {"diet.f3", "Show F3 debug screen (F3 toggles)"},
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
        {"diet.skip", "遮蔽判定されたターゲットの描画をスキップ"},
        {"diet.probe", "レンダープローブ（診断用・除外はしない）"},
        {"diet.distance", "距離カリング（ブロック）"},
        {"diet.tick_budget", "カリング処理のティック予算 (ms)"},
        {"diet.measure", "フレームスパイクとパケット到着の相関を計測"},
        {"diet.net_budget", "この時間を超えたフレームでは Diet の処理を休止 (ms)（直近平均の2.5倍も同様）"},
        {"diet.defer", "遅延許容パケットの処理を毎ティックに分散（ブロック更新系のみ）"},
        {"diet.defer_budget", "1ティックで戻す遅延パケット数"},
        {"diet.f3", "F3 デバッグ画面を表示（F3 で切り替え）"},
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
                        .add(Setting::toggle("cull_skip_draw", "diet.skip", true))
                        .add(Setting::toggle("cull_probe", "diet.probe", false))
                        .add(Setting::toggle("net_measure", "diet.measure", true))
                        .add(Setting::slider("net_budget_ms", "diet.net_budget", 3.0, 0.5, 16.0, 0.5))
                        .add(Setting::toggle("net_defer", "diet.defer", true))
                        .add(Setting::slider("net_defer_budget", "diet.defer_budget", 8, 1, 64, 1))
                        .add(Setting::toggle("f3_enabled", "diet.f3", false))
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
            "cull_max_distance", "tick_budget_ms", "cull_debug_draw", "cull_skip_draw",
            "cull_probe", "net_measure", "net_budget_ms", "net_defer", "net_defer_budget",
            "f3_enabled", "debug"};
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
// Also publishes the F3 debug-screen snapshot at 4 Hz.
// ---------------------------------------------------------------------------
std::string gGameVersion;

std::shared_ptr<const diet::F3Snapshot> buildF3Snapshot(const Config& config) {
    auto snapshot = std::make_shared<diet::F3Snapshot>();
    snapshot->gameVersion = gGameVersion;
    snapshot->gameVersionKnown = !gGameVersion.empty();

    TickReportData report{};
    {
        std::lock_guard lock(tickReport.mutex);
        report = tickReport.data;
    }
    const auto now = steadyNs();
    auto traffic = runtime->traffic.snapshot(now);

    snapshot->fpsValid = traffic.frames > 0;
    snapshot->fps = traffic.fps;
    snapshot->frameStatsValid = traffic.frames > 0;
    snapshot->frameP50Ms = traffic.frameMsP50;
    snapshot->frameP95Ms = traffic.frameMsP95;
    snapshot->frameMaxMs = traffic.frameMsMax;

    snapshot->positionValid = runtime->poseValid.load(std::memory_order_relaxed);
    snapshot->x = runtime->poseX.load(std::memory_order_relaxed);
    snapshot->y = runtime->poseY.load(std::memory_order_relaxed);
    snapshot->z = runtime->poseZ.load(std::memory_order_relaxed);
    snapshot->facingValid = snapshot->positionValid;
    snapshot->yawDeg = runtime->poseYaw.load(std::memory_order_relaxed);
    snapshot->pitchDeg = runtime->posePitch.load(std::memory_order_relaxed);

    snapshot->cullValid = config.cull;
    snapshot->tracked = report.engine.tracked;
    snapshot->hidden = report.engine.hidden;
    snapshot->checked = report.engine.checked;
    snapshot->samples = report.engine.samples;
    snapshot->budgetStops = report.engine.budgetStops;
    snapshot->blockQueries = runtime->blockQueries.load(std::memory_order_relaxed);
    // The counter is only meaningful when the render filter actually installed:
    // a hidden verdict without a wired exclusion changes nothing on screen.
    snapshot->skippedDrawsValid =
        config.cull && config.skipDraw && !config.renderProbe &&
        clip.actorFilter && clip.actorFilter->ready();
    snapshot->skippedDraws = skippedDraws.load(std::memory_order_relaxed);

    snapshot->trafficValid = true;
    snapshot->packets = traffic.packets;
    snapshot->deferValid =
        config.defer && clip.packetDeferral && clip.packetDeferral->ready();
    snapshot->deferredPackets = deferredTotal.load(std::memory_order_relaxed);
    snapshot->deferredQueue =
        clip.packetDeferral ? clip.packetDeferral->queueDepth() : 0;
    snapshot->pingValid = traffic.latencyValid && traffic.latencyMs >= 0.0;
    snapshot->pingMs = traffic.latencyValid
        ? static_cast<unsigned>(traffic.latencyMs + 0.5)
        : 0u;
    // Top four non-empty packet classes in this window, Java-style "name=count".
    for (int slot = 0; slot < 4; ++slot) {
        std::uint64_t best = 0;
        diet::PacketClass bestClass = diet::PacketClass::Unknown;
        for (std::size_t c = 0; c < traffic.classPackets.size(); ++c) {
            if (traffic.classPackets[c] > best) {
                best = traffic.classPackets[c];
                bestClass = static_cast<diet::PacketClass>(c);
            }
        }
        if (!best) break;
        if (!snapshot->packetClasses.empty()) snapshot->packetClasses += ' ';
        snapshot->packetClasses += diet::packetClassName(bestClass);
        snapshot->packetClasses += '=';
        snapshot->packetClasses += std::to_string(best);
        // Zero the class we just showed so the next pass picks the next one.
        traffic.classPackets[static_cast<std::size_t>(bestClass)] = 0;
    }

    snapshot->system = diet::collectSystemInfo();
    return snapshot;
}

void reportLoop(std::atomic<bool>& stopping) {
    unsigned sinceLog = 0;
    while (!stopping.load(std::memory_order_acquire)) {
        for (int i = 0; i < 5 && !stopping.load(std::memory_order_acquire); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (stopping.load(std::memory_order_acquire)) break;
        if (!runtime) continue;
        const auto config = configCopy();
        runtime->debugScreen.publish(buildF3Snapshot(config));
        if (clip.actorFilter) clip.actorFilter->setProbe(config.renderProbe);
        if (++sinceLog < 20) continue;  // 5 s cadence for the log lines
        sinceLog = 0;
        if (config.measure) {
            const auto text = runtime->traffic.buildReport(steadyNs());
            logger.info(text);
        }
        TickReportData copy{};
        {
            std::lock_guard lock(tickReport.mutex);
            copy = tickReport.data;
        }
        const auto trafficNow = runtime->traffic.snapshot(steadyNs());
        const double pingMsForLog = trafficNow.latencyValid ? trafficNow.latencyMs : -1.0;
        unsigned filterCalls = 0, filterVariants = 0, filterExcluded = 0, filterMatches = 0;
        if (clip.actorFilter) {
            const auto statistics = clip.actorFilter->takeStatistics();
            filterCalls = statistics.calls;
            filterExcluded = statistics.excluded;
            filterMatches = statistics.matches;
            filterVariants = static_cast<unsigned>(statistics.variants);
            skippedDraws.fetch_add(statistics.excluded, std::memory_order_relaxed);
        }
        bedrock::sdk::PacketDeferralStatistics deferral{};
        if (clip.packetDeferral) {
            deferral = clip.packetDeferral->takeStatistics();
            deferredTotal.fetch_add(deferral.deferred, std::memory_order_relaxed);
        }
        std::ostringstream chunkHandlers;
        for (const auto& handler : deferral.byHandler) {
            if (handler.packetId != 58 && handler.packetId != 172) continue;
            chunkHandlers << " id" << handler.packetId << "_calls=" << handler.calls
                          << " id" << handler.packetId << "_held=" << handler.deferred
                          << " id" << handler.packetId << "_replayed=" << handler.replayed;
        }
        logger.info("cull summary: tracked=", copy.engine.tracked,
                    " checked=", copy.engine.checked, " samples=", copy.engine.samples,
                    " visible=", copy.engine.visible, " occluded=", copy.engine.occluded,
                    " distance_culled=", copy.engine.distanceCulled,
                    " protected=", copy.engine.protectedTargets,
                    " budget_stops=", copy.engine.budgetStops, " hidden=", copy.engine.hidden,
                    " skipped=", skippedDraws.load(std::memory_order_relaxed),
                     " filter_calls=", filterCalls, " filter_excluded=", filterExcluded,
                     " filter_matches=", filterMatches, " filter_variants=", filterVariants,
                    " engine_us=", copy.engine.elapsedUs, " capture_rows=", copy.captureRows,
                    " population=", copy.capturePopulation, " capture_us=", copy.captureUs,
                    " block_queries=", runtime->blockQueries.load(std::memory_order_relaxed),
                    " ping_ms=", pingMsForLog,
                    " lat_obs=", trafficNow.latencyObservations,
                    " lat_pairs=", trafficNow.latencyPairs,
                    " deferred=", deferral.deferred, " replayed=", deferral.replayed,
                    " queue=", deferral.queue, " defer_calls=", deferral.calls,
                    " defer_overflow=", deferral.overflow,
                    " defer_other_object=", deferral.otherObject,
                    " defer_class_filtered=", deferral.otherClass,
                    " defer_aged=", deferral.agedOut,
                    " defer_world_drops=", deferral.worldDrops,
                    " defer_total=", deferredTotal.load(std::memory_order_relaxed),
                    chunkHandlers.str(),
                    " deferred_ticks=", runtime->deferredTicks.load(std::memory_order_relaxed),
                    " ticks=", copy.ticks,
                    " f3_visible=", runtime->debugScreen.visible() ? 1 : 0,
                    " f3_paints=", runtime->debugScreen.paintCount(),
                    " key_events=", keyEvents.load(std::memory_order_relaxed),
                    " last_key=", lastKey.load(std::memory_order_relaxed),
                    " raw_id_min=", runtime->rawIdMin.load(std::memory_order_relaxed),
                    " raw_id_max=", runtime->rawIdMax.load(std::memory_order_relaxed),
                    " raw_id_last=", runtime->lastRawId.load(std::memory_order_relaxed));
        if (clip.actorFilter && config.renderProbe) {
            const auto samples = clip.actorFilter->takeProbeSamples();
            unsigned shown = 0;
            for (const auto& sample : samples) {
                if (shown++ >= 12) break;
                std::ostringstream line;
                line << "render probe: table=" << hexPointer(sample.table)
                     << " object=" << hexPointer(sample.object)
                     << " context=" << hexPointer(sample.context) << " calls=" << sample.calls;
                for (unsigned word = 0; word < bedrock::sdk::ActorRenderProbeSample::kSampleWords; ++word) {
                    if (!sample.words[word]) continue;
                    line << " w" << word << '=' << hexPointer(sample.words[word]);
                }
                const unsigned trackedNow = trackedPointerCount.load(std::memory_order_acquire);
                for (unsigned word = 0; word < bedrock::sdk::ActorRenderProbeSample::kSampleWords; ++word) {
                    if (!sample.words[word]) continue;
                    for (unsigned actor = 0; actor < trackedNow && actor < kMaxTrackedPointers;
                         ++actor) {
                        if (sample.words[word] !=
                            trackedPointers[actor].load(std::memory_order_relaxed))
                            continue;
                        line << " MATCH_ACTOR word=" << word;
                    }
                }
                for (unsigned word = 0; word < bedrock::sdk::ActorRenderProbeSample::kContextWords; ++word) {
                    if (!sample.contextWords[word]) continue;
                    line << " c" << word << '=' << hexPointer(sample.contextWords[word]);
                }
                logger.info(line.str());
            }
            if (samples.empty()) logger.info("render probe: no watched-pass objects this window");
            const unsigned tracked = trackedPointerCount.load(std::memory_order_acquire);
            if (tracked) {
                std::ostringstream line;
                line << "render probe tracked actors:";
                for (unsigned index = 0; index < tracked && index < kMaxTrackedPointers; ++index)
                    line << ' ' << hexPointer(trackedPointers[index].load(std::memory_order_relaxed));
                logger.info(line.str());
            }
            const unsigned hidden = hiddenPointerCount.load(std::memory_order_acquire);
            if (hidden) {
                std::ostringstream line;
                line << "render probe hidden actors:";
                for (unsigned index = 0; index < hidden && index < kMaxPublishedPointers; ++index)
                    line << ' ' << hexPointer(hiddenPointers[index].load(std::memory_order_relaxed));
                logger.info(line.str());
            }
        }
    }
}


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

    // Diagnostic overrides for the BOL harness: the OreUI Mod Settings store is
    // unavailable before the game UI exists, so these two marker files let a
    // test run turn on the render probe / the debug boxes without a UI. They
    // never change a shipped default.
    std::error_code markerError;
    const bool probeMarker = std::filesystem::exists(directory / L"Diet.probe", markerError);
    markerError.clear();
    const bool renderProbeMarker =
        std::filesystem::exists(directory / L"Diet.renderprobe", markerError);
    markerError.clear();
    const bool drawMarker = std::filesystem::exists(directory / L"Diet.debugdraw", markerError);
    if (probeMarker || renderProbeMarker || drawMarker) {
        std::lock_guard lock(configMutex);
        liveConfig.renderProbe = probeMarker || renderProbeMarker;
        if (probeMarker) {
            // Logging-only stage: the packet handlers are detoured and counted,
            // nothing is held. Removing the marker is the deferral stage.
            deferProbeOnly.store(true);
        }
        if (drawMarker) {
            liveConfig.debugDraw = true;
            renderDebugDraw.store(true);
        }
        logger.info("diagnostic markers: probe=", liveConfig.renderProbe ? 1 : 0,
                    " debug_draw=", drawMarker ? 1 : 0,
                    " defer_probe_only=", probeMarker ? 1 : 0);
    }

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
    clip.subscriptions.emplace_back(clip.hooks->onKey(onKey));
    clip.subscriptions.emplace_back(clip.hooks->onNativeKey(onNativeKey));
    if (!clip.hooks->install(resolver, profile.json(), error)) {
        logger.error("hook install failed: ", error);
    } else {
        logger.info("hooks active: level tick, gameplay, world change");
        std::string inputError;
        if (!clip.hooks->installInput(resolver, inputError))
            logger.warn("keyboard input unavailable (F3 key): ", inputError);
        else
            logger.info("keyboard input active (F3 toggles the debug screen)");
        std::string nativeInputError;
        if (!clip.hooks->installNativeKeyboard(resolver, profile.json(), nativeInputError))
            logger.warn("native keyboard events unavailable: ", nativeInputError);
        else
            logger.info("native keyboard events active (F3 second route)");
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

    // Per-actor render skip: the SDK verifies the live callable tables and the
    // shared predicate before it detours anything, so a profile mismatch leaves
    // the game completely untouched.
    {
        const auto config = configCopy();
        clip.actorFilter = std::make_unique<bedrock::sdk::ActorRenderFilter>();
        clip.actorFilter->setProbe(config.renderProbe);
        std::vector<std::string> warnings;
        std::string filterError;
        if (!clip.actorFilter->configure(profile, &shouldExcludeRenderObject, filterError, warnings)) {
            logger.warn("render skip unavailable: ", filterError);
            clip.actorFilter.reset();
        } else {
            for (const auto& warning : warnings) logger.info(warning);
            logger.info("render filter ready; diagnostics_only=", config.renderProbe ? 1 : 0);
        }
    }

    // Packet deferral: the SDK verifies the authenticated handler entries, each
    // one's vtable slot, the callable table of its queued lambda and the
    // shared_ptr control-block layout before it detours anything. Only the
    // profile-listed delay-tolerant classes can be held.
    {
        const auto config = configCopy();
        clip.packetDeferral = std::make_unique<bedrock::sdk::PacketDeferral>();
        std::vector<std::string> warnings;
        std::string deferralError;
        if (!clip.packetDeferral->configure(profile, deferralError, warnings, &diet::mayDefer)) {
            logger.warn("packet deferral unavailable: ", deferralError);
            clip.packetDeferral.reset();
        } else {
            for (const auto& warning : warnings) logger.info(warning);
            syncDeferral();
            logger.info("packet deferral armed=", clip.packetDeferral->enabled() ? 1 : 0,
                        " budget=", config.deferBudget, " tick");
        }
    }

    gGameVersion = profile.version();
    if (!runtime->debugScreen.install(error)) {
        logger.warn("F3 debug screen unavailable: ", error);
    } else {
        runtime->debugScreen.setVisible(configCopy().f3);
        logger.info("F3 debug screen active (version line: ", gGameVersion, ")");
    }

    std::atomic<bool> stopping{false};
    std::thread reporter(reportLoop, std::ref(stopping));
    gStatus.store(4);
    logger.info("ready; packet deferral holds the "
                "UpdateBlock-family shared_ptr handlers and replays them per tick "
                "(evidence-11); ping from NetworkStackLatencyPacket");

    while (!gStop.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    stopping.store(true, std::memory_order_release);
    reporter.join();
    runtime->debugScreen.uninstall();
    clip.actorFilter.reset();
    if (clip.packetDeferral) {
        clip.packetDeferral->setEnabled(false);
        (void)clip.packetDeferral->clear();
    }
    clip.packetDeferral.reset();
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
