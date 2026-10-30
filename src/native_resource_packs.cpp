#include <bedrock/native/resource_packs.hpp>
#include <bedrock/api/resource_packs.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <bedrock/detail/owned_msvc_string.hpp>
#include <bedrock/detail/resource_reload_barrier.hpp>
#include <cstdio>
#include <memory>
#include <intrin.h>
#include <bedrock/detail/pending_terrain_refresh.hpp>
#include <bedrock/detail/scoped_suspend_gate.hpp>
#include <bedrock/detail/native_material_uvs.hpp>

#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
#error NativeResourcePacks requires the Release MSVC STL ABI (_ITERATOR_DEBUG_LEVEL=0)
#endif

namespace bedrock {
struct NativeResourcePacks::Binding {
    using Initialize = void(__fastcall *)(void *);
    using Exit = std::uint64_t(__fastcall *)(void *);
    using Event = void(__fastcall *)(void *, void *);
    using Compose = std::uint64_t(__fastcall *)(void *, void *, void *, void *);
    using Construct = void *(__fastcall *)(void *, void *, bool, bool, const void *, bool);
    using Predicate = bool(__fastcall *)(void *);
    using ScreenName = void *(__fastcall *)(void *, void *);
    using LocalPlayer = void *(__fastcall *)(void *);
    using ClientGame = void *(__fastcall *)(void *);
    using GraphicsGetter = void *(__fastcall *)(const void *);
    using AtlasUpdate = void(__fastcall *)(void *, const void *, bool);
    using Deallocate = void(__fastcall *)(void *);
    void *initTarget{};
    void *ctorTarget{};
    void *preGameTarget{};
    void *exitTarget{}, *eventTarget{}, *composeTarget{};
    void *navigateTarget{}, *progressTickTarget{};
    Exit progressTick{};
    std::size_t progressModelOffset{}, progressCallbackOffset{}, progressInvokeOffset{};
    std::atomic<std::uint64_t> previousReloadWaitFrames{};
    std::atomic<bool> progressWaiting{};
    void *atlasTarget{}, *atlasCaller{}, *frameTarget{};
    Initialize frame{};
    Event cameraFrame{}, cameraRebind{}, initializeBlockGraphics{}, cachedGraphics{};
    GraphicsGetter blockGraphics{};
    void *cameraFrameTarget{}, *blockGraphicsTarget{}, *cachedGraphicsTarget{};
    Initialize platformSuspend{}, platformResume{};
    void *gamePreSuspendTarget{}, *gameSuspendTarget{}, *gameResumeTarget{};
    void *suspendGateTarget{}, *suspendGateCaller1{}, *suspendGateCaller2{};
    Predicate suspendGate{};
    Predicate remoteClient{};
    std::size_t gameListenerOffset{}, gamePreSlot{}, gameSuspendSlot{}, gameResumeSlot{};
    std::atomic<std::uint64_t> remoteReloads{}, suspendGateOverrides{};
    std::atomic<NativeResourcePacks::TraceCallback> traceCallback{};
    void trace(const char *message) const noexcept {
        if (auto callback = traceCallback.load(std::memory_order_acquire)) callback(message);
    }
    static bool __fastcall onSuspendGate(void *game) {
        auto &state = *active().load(std::memory_order_acquire);
        const void *caller = _ReturnAddress();
        if (detail::ScopedSuspendGate::matches(state.enabled.load(std::memory_order_acquire), game,
                                               caller, state.suspendGateCaller1, state.suspendGateCaller2)) {
            ++state.suspendGateOverrides;
            state.trace("suspend_gate_override\r\n");
            return true;
        }
        return state.suspendGate(game);
    }
    void *platformGlobal{};
    std::size_t platformObjectOffset{}, platformSuspendSlot{}, platformResumeSlot{};
    std::size_t cameraOwnerOffset{}, rendererCameraOffset{}, cameraDimensionOffset{}, playerDimensionOffset{}, playerDimensionControlOffset{}, cameraFrameSlot{}, cachedGraphicsOffset{};
    std::atomic<bool> freshBlockGraphics{};
    std::atomic<std::uint64_t> blockGraphicsRebuilt{}, graphicsCacheResets{}, platformCycles{}, camerasRefreshed{};
    detail::PendingCameraRefresh pendingCamera;
    ClientGame clientRenderer{};
    Initialize drainRendererTasks{};
    std::size_t rendererTasksOffset{};
    std::size_t clientsOffset{}, nodeLeftOffset{}, nodeNilOffset{}, nodeKeyOffset{}, nodeClientOffset{};
    std::size_t loaderOffset{}, loaderReadyOffset{}, loaderOutstandingOffset{};
    std::atomic<std::uint64_t> resourceWaitFrames{};
    using GeometryReload = void(__fastcall *)(void *, bool);
    GeometryReload geometryReload{};
    detail::NativeMaterialUvs materialUvs;
    std::atomic<std::uint64_t> customMaterialsRemapped{}, customUvSetsUpdated{}, customUvMismatched{}, customUvUnmatched{}, customUvUnchanged{};
    Event notifyClientResources{};
    std::atomic<std::uint64_t> geometryReloaded{}, clientDefinitionsReloaded{};
    AtlasUpdate atlasUpdate{};
    ClientGame clientLevel{}, levelData{}, gamePacks{};
    std::size_t experimentsOffset{}, terrainAtlasOffset{};
    std::mutex refreshMutex;
    detail::PendingTerrainRefresh pendingRefresh;
    detail::ResourceReloadBarrier reloadBarrier;
    std::atomic<std::uint64_t> nativeScreenWaitFrames{};
    std::atomic<bool> refreshPending{};
    std::atomic<std::uint64_t> terrainCompleted{}, refreshCancelled{};
    Event navigate{};
    Exit exit{};
    Event event{};
    Compose compose{};
    Initialize initialize{};
    Construct construct{};
    Predicate preGame{};
    ScreenName screenName{};
    LocalPlayer localPlayer{};
    ClientGame clientGame{};
    void *gameInGameTarget{}, *gameGateCaller{};
    Predicate gameInGame{};
    Deallocate deallocate{};
    std::size_t reloadOffset{}, stringSizeOffset{}, stringCapacityOffset{}, stringInlineBytes{},
        stringObjectBytes{}, maximumStringBytes{}, largeAllocationThreshold{}, alignmentOverhead{};
    std::size_t closureOwnerOffset{}, settingsModelOffset{}, modelClientOffset{}, includeGlobalOffset{};
    std::array<BDS_HookToken, 15> tokens{};
    std::atomic<bool> enabled{}, installed{};
    bool retained{};
    std::atomic<std::uint64_t> opened{}, overrides{}, failures{};
    std::atomic<std::uint64_t> exits{}, applyEvents{}, compositions{}, globalIncluded{};
    std::atomic<std::uint64_t> navigations{}, liveApplications{}, inWorldEvents{};
    std::atomic<std::uint64_t> gameGateOverrides{};
    std::mutex policyMutex;
    std::map<void *, ResourcePackMenuPolicy> policies;

    static std::atomic<Binding *> &active() {
        static std::atomic<Binding *> value{};
        return value;
    }
    static std::mutex &installationMutex() {
        static std::mutex value;
        return value;
    }

    static void *&applyingClient() {
        static thread_local void *client{};
        return client;
    }
    struct ApplyScope {
        void *previous;
        explicit ApplyScope(void *client) : previous(applyingClient()) {
            if (client)
                applyingClient() = client;
        }
        ~ApplyScope() { applyingClient() = previous; }
    };
    void *clientFromModel(void *model) const noexcept {
        void *client{};
        return model && native_profile::read(model, modelClientOffset, client) ? client : nullptr;
    }
    static bool __fastcall onGameInGame(void *game) {
        // The game has a second gate independent of ClientInstance::isPreGame.
        // Scope the override to the one call in onFullPackStackInvalid. Other
        // callers, other game instances and the remaining native readiness
        // checks retain their original behavior.
        const auto caller = _ReturnAddress();
        auto &state = *active().load(std::memory_order_acquire);
        const bool original = state.gameInGame(game);
        const auto client = applyingClient();
        if (original && state.enabled.load(std::memory_order_acquire) &&
            caller == state.gameGateCaller && client &&
            state.clientGame(client) == game && state.localPlayer(client)) {
            ++state.gameGateOverrides;
            return false;
        }
        return original;
    }
    static void __fastcall onAtlasUpdate(void *game, const void *atlas, bool terrain) {
        const auto caller = _ReturnAddress();
        auto &state = *active().load(std::memory_order_acquire);
        state.atlasUpdate(game, atlas, terrain);
        if (!state.enabled.load(std::memory_order_acquire) || !terrain || caller != state.atlasCaller)
            return;
        void *currentAtlas{};
        if (!native_profile::read(game, state.terrainAtlasOffset, currentAtlas) || currentAtlas != atlas)
            return;
        std::lock_guard lock(state.refreshMutex);
        if (state.pendingRefresh.complete(game)) {
            ++state.terrainCompleted;
            state.trace("terrain_atlas_ready\r\n");
        }
    }
    static bool &readingBlockGraphics() {
        static thread_local bool value{};
        return value;
    }
    static void *__fastcall onBlockGraphics(const void *block) {
        auto &state = *active().load(std::memory_order_acquire);
        const bool previous = readingBlockGraphics();
        struct Restore { bool previous; ~Restore() { readingBlockGraphics() = previous; } } restore{previous};
        readingBlockGraphics() = true;
        return state.blockGraphics(block);
    }
    static bool clearGraphics(void *data, std::size_t offset, void *&previous) noexcept {
        __try {
            previous = InterlockedExchangePointer(
                reinterpret_cast<void *volatile *>(static_cast<std::byte *>(data) + offset), nullptr);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static void __fastcall onCachedGraphics(void *capture, void *data) {
        auto &state = *active().load(std::memory_order_acquire);
        // This reader is shared with other game systems. Only alter the
        // BlockGraphics::getForBlock path, as in the Android mod.
        if (readingBlockGraphics() && state.enabled.load(std::memory_order_acquire) &&
            state.freshBlockGraphics.load(std::memory_order_acquire)) {
            void *previous{};
            if (!clearGraphics(data, state.cachedGraphicsOffset, previous)) ++state.failures;
            else if (previous) {
                const auto count = ++state.graphicsCacheResets;
                if (count == 1) state.trace("graphics_cache_reset first\r\n");
                else if (count == 1024) state.trace("graphics_cache_reset count=1024\r\n");
                else if (count == 4096) state.trace("graphics_cache_reset count=4096\r\n");
            }
        }
        state.cachedGraphics(capture, data);
    }
    static void __fastcall onCameraFrame(void *camera, void *frameContext) {
        auto &state = *active().load(std::memory_order_acquire);
        const bool rebound = state.enabled.load(std::memory_order_acquire) && state.refreshCamera(camera);
        if (rebound) state.trace("camera_frame_original begin\r\n");
        state.cameraFrame(camera, frameContext);
        if (rebound) state.trace("camera_frame_original end\r\n");
    }
    bool refreshCamera(void *camera) {
        std::unique_lock lock(refreshMutex);
        auto *game = pendingCamera.game();
        if (!game || pendingCamera.thread() != GetCurrentThreadId()) return false;
        auto *client = primaryClient(game);
        auto *player = client && clientGame(client) == game ? localPlayer(client) : nullptr;
        auto *level = player ? clientLevel(client) : nullptr;
        if (pendingCamera.cancelIfChanged(level)) { ++refreshCancelled; trace("camera_context_changed\r\n"); return false; }
        if (!player || !level) return false;
        auto *renderer = clientRenderer(client);
        void *owner{}, *currentCamera{}, *dimension{}, *playerDimension{}, *control{}, *vtable{}, *frameEntry{};
        if (!renderer || !native_profile::read(camera, cameraOwnerOffset, owner) || owner != renderer ||
            !native_profile::read(renderer, rendererCameraOffset, currentCamera) || currentCamera != camera ||
            !native_profile::read(camera, cameraDimensionOffset, dimension) || !dimension ||
            !native_profile::read(player, playerDimensionOffset, playerDimension) || !playerDimension ||
            !native_profile::read(player, playerDimensionControlOffset, control) || !control ||
            !native_profile::read(camera, 0, vtable) || !native_profile::read(vtable, cameraFrameSlot, frameEntry) ||
            frameEntry != cameraFrameTarget) return false;
        // Claim before invoking native code to prevent a nested frame from
        // rebinding twice. Pointers are borrowed only for this call.
        pendingCamera.clear();
        lock.unlock();
        trace("camera_rebind begin\r\n");
        cameraRebind(camera, player);
        trace("camera_rebind end\r\n");
        ++camerasRefreshed;
        return true;
    }
    void *primaryClient(void *game) const noexcept {
        // Borrow the primary map entry only during startFrame on its owning
        // thread. This is the same key-zero lookup as the native App getter.
        void *head{}, *node{}, *client{};
        unsigned char nil{}, key{};
        if (!native_profile::read(game, clientsOffset, head) || !head ||
            !native_profile::read(head, nodeLeftOffset, node) || !node || node == head ||
            !native_profile::read(node, nodeNilOffset, nil) || nil ||
            !native_profile::read(node, nodeKeyOffset, key) || key != 0 ||
            !native_profile::read(node, nodeClientOffset, client))
            return nullptr;
        return client;
    }
    bool resourcesReady(void *game) const noexcept {
        void *loader{};
        unsigned char ready{};
        std::uint64_t outstanding{};
        return native_profile::read(game, loaderOffset, loader) && loader &&
               native_profile::read(loader, loaderReadyOffset, ready) && ready == 1 &&
               native_profile::read(loader, loaderOutstandingOffset, outstanding) && outstanding == 0;
    }
    bool idleWorldScreen(void *client) {
        try { return client && detail::isWorldRenderScreen(screen(client)); }
        catch (const std::exception &) { return false; }
    }
    bool reloadIdentity(void *game, void *&loader, void *&atlas) const noexcept {
        return resourcesReady(game) && native_profile::read(game, loaderOffset, loader) && loader &&
            native_profile::read(game, terrainAtlasOffset, atlas) && atlas;
    }
    void observeCompletedFrame(void *game) {
        if (!enabled.load(std::memory_order_acquire) || !refreshPending.load(std::memory_order_acquire)) return;
        auto *client = primaryClient(game);
        auto *level = client && clientGame(client) == game && localPlayer(client) ? clientLevel(client) : nullptr;
        void *loader{}, *atlas{};
        const bool idle = level && reloadIdentity(game, loader, atlas) && idleWorldScreen(client);
        std::lock_guard lock(refreshMutex);
        const bool stable = idle && pendingRefresh.atlasReady();
        const bool wasReady = reloadBarrier.allows(pendingRefresh.generation(), loader, atlas, stable);
        reloadBarrier.observe(pendingRefresh.generation(), loader, atlas, stable);
        if (stable && !wasReady) trace("native_reload_world_frame_completed\r\n");
    }
    void refreshAtFrame(void *game) {
        if (!enabled.load(std::memory_order_acquire) ||
            !refreshPending.load(std::memory_order_acquire))
            return;
        auto *client = primaryClient(game);
        auto *player = client ? localPlayer(client) : nullptr;
        auto *level = player && clientGame(client) == game ? clientLevel(client) : nullptr;
        void *manager{};
        bool cancelled{};
        void *loader{}, *atlas{};
        const bool ready = reloadIdentity(game, loader, atlas);
        const bool worldScreen = ready && idleWorldScreen(client);
        bool barrierReady{};
        {
            std::lock_guard lock(refreshMutex);
            barrierReady = reloadBarrier.allows(pendingRefresh.generation(), loader, atlas, ready && worldScreen);
            if (!barrierReady) reloadBarrier.clear();
            manager = pendingRefresh.take(game, level, GetCurrentThreadId(), cancelled, ready && barrierReady);
            refreshPending.store(pendingRefresh.pending(), std::memory_order_release);
        }
        if (cancelled)
            ++refreshCancelled;
        if (!manager) {
            if (!ready) ++resourceWaitFrames;
            else if (!barrierReady) {
                const auto waits = ++nativeScreenWaitFrames;
                if (waits == 1) trace("refresh_wait_native_reload_screen\r\n");
            }
            return;
        }
        trace("refresh_claimed\r\n");
        auto *data = levelData(level);
        if (!data || gamePacks(game) != manager) {
            ++failures;
            return;
        }
        auto *renderer = clientRenderer(client);
        if (!renderer) {
            ++failures;
            return;
        }
        void *tasks{}, *platform{}, *lifetime{}, *vtable{}, *suspendEntry{}, *resumeEntry{};
        unsigned char live{};
        if (!native_profile::read(renderer, rendererTasksOffset, tasks) || !tasks ||
            !native_profile::read(platformGlobal, 0, lifetime) || !lifetime ||
            !native_profile::read(lifetime, 0, live) || live != 1 ||
            !native_profile::read(platformGlobal, platformObjectOffset, platform) || !platform ||
            !native_profile::read(platform, 0, vtable) ||
            !native_profile::read(vtable, platformSuspendSlot, suspendEntry) ||
            !native_profile::read(vtable, platformResumeSlot, resumeEntry) ||
            suspendEntry != reinterpret_cast<void *>(platformSuspend) ||
            resumeEntry != reinterpret_cast<void *>(platformResume)) {
            ++failures;
            return;
        }
        // Android calls the platform suspension/resumption notifications after
        // finalization, then rebuilds BlockGraphics. Drain mesh work first;
        // Windows has asynchronous renderer jobs and this object is borrowed.
        const bool remote = remoteClient(client);
        void *gameListener = static_cast<std::byte *>(game) + gameListenerOffset;
        if (remote) {
            void *gameTable{}, *pre{}, *suspend{}, *resume{};
            if (!native_profile::read(gameListener, 0, gameTable) ||
                !native_profile::read(gameTable, gamePreSlot, pre) || pre != gamePreSuspendTarget ||
                !native_profile::read(gameTable, gameSuspendSlot, suspend) || suspend != gameSuspendTarget ||
                !native_profile::read(gameTable, gameResumeSlot, resume) || resume != gameResumeTarget) {
                ++failures;
                trace("game_listener_validation_failed\r\n");
                return;
            }
            ++remoteReloads;
            trace("mode_remote_scoped_gate\r\n");
        } else trace("mode_local_unfiltered\r\n");
        trace("drain_before_platform begin\r\n");
        drainRendererTasks(tasks);
        trace("drain_before_platform end\r\n");
        {
            // Scope only the mod-initiated synchronous broadcast on this
            // thread. Normal operating-system lifecycle events still pass.
            detail::ScopedSuspendGate scope(remote ? game : nullptr);
            trace("platform_suspend begin\r\n");
            platformSuspend(platform);
            trace("platform_suspend end\r\nplatform_resume begin\r\n");
            platformResume(platform);
            trace("platform_resume end\r\n");
        }
        ++platformCycles;
        // Lifecycle callbacks can replace client or renderer state. Recheck
        // before touching the old graphics registry and drain any newly queued
        // mesh work created by resume.
        if (primaryClient(game) != client || clientGame(client) != game ||
            clientLevel(client) != level || gamePacks(game) != manager ||
            clientRenderer(client) != renderer ||
            !native_profile::read(renderer, rendererTasksOffset, tasks) || !tasks) {
            ++refreshCancelled;
            return;
        }
        trace("drain_before_block_init begin\r\n");
        drainRendererTasks(tasks);
        trace("drain_before_block_init end\r\n");
        // v6 restored player models with the game's geometry reload and
        // per-client notification. Keep the v15 session gate and validate the
        // borrowed world/renderer after each native operation.
        trace("geometry_reload begin\r\n");
        geometryReload(game, true);
        ++geometryReloaded;
        trace("geometry_reload end\r\n");
        if (primaryClient(game) != client || clientGame(client) != game ||
            clientLevel(client) != level || gamePacks(game) != manager ||
            clientRenderer(client) != renderer) {
            ++refreshCancelled;
            trace("geometry_context_changed\r\n");
            return;
        }
        trace("notify_client begin\r\n");
        std::array<void *, 2> closure{nullptr, game};
        notifyClientResources(closure.data(), client);
        ++clientDefinitionsReloaded;
        trace("notify_client end\r\n");
        if (primaryClient(game) != client || clientGame(client) != game ||
            clientLevel(client) != level || gamePacks(game) != manager ||
            clientRenderer(client) != renderer || !(data = levelData(level))) {
            ++refreshCancelled;
            trace("client_context_changed\r\n");
            return;
        }
        freshBlockGraphics.store(true, std::memory_order_release);
        trace("init_block_graphics begin\r\n");
        initializeBlockGraphics(manager, static_cast<std::byte *>(data) + experimentsOffset);
        trace("init_block_graphics end\r\n");
        ++blockGraphicsRebuilt;
        // UV-only repair: keep shared Material objects and vector allocations.
        // The existing camera refresh below rebuilds chunks from these values.
        trace("drain_before_custom_uv begin\r\n");
        if (clientRenderer(client) != renderer ||
            !native_profile::read(renderer, rendererTasksOffset, tasks) || !tasks) {
            ++refreshCancelled;
            trace("custom_uv_context_changed\r\n");
            return;
        }
        drainRendererTasks(tasks);
        trace("drain_before_custom_uv end\r\ncustom_uv_prepare begin\r\n");
        materialUvs.counts = {};
        try {
            void *atlas{};
            if (!native_profile::read(game, terrainAtlasOffset, atlas) || !atlas)
                throw std::runtime_error("custom_uv_no_atlas");
            auto *atlasManager = materialUvs.atlasManager(atlas);
            if (!materialUvs.prepare(atlasManager))
                throw std::runtime_error("custom_uv_snapshot_rejected");
            trace("custom_uv_prepare end\r\n");
            void *currentAtlas{};
            if (primaryClient(game) != client || clientGame(client) != game ||
                clientLevel(client) != level || gamePacks(game) != manager ||
                clientRenderer(client) != renderer || !resourcesReady(game) ||
                !native_profile::read(game, terrainAtlasOffset, currentAtlas) || currentAtlas != atlas ||
                materialUvs.atlasManager(atlas) != atlasManager)
                throw std::runtime_error("custom_uv_context_changed");
            trace("custom_uv_commit begin\r\n");
            if (!materialUvs.commit()) throw std::runtime_error("custom_uv_commit_rejected");
            trace("custom_uv_commit end\r\n");
            customMaterialsRemapped += materialUvs.counts.materials;
            customUvSetsUpdated += materialUvs.counts.rows;
        } catch (const std::exception &error) {
            ++failures;
            trace(error.what());
            trace("\r\n");
        }
        char uvSummary[256]{};
        _snprintf_s(uvSummary, sizeof(uvSummary), _TRUNCATE,
            "custom_uv_result materials=%llu rows=%llu mismatched=%llu unmatched=%llu unchanged=%llu\r\n",
            static_cast<unsigned long long>(materialUvs.counts.materials),
            static_cast<unsigned long long>(materialUvs.counts.rows),
            static_cast<unsigned long long>(materialUvs.counts.mismatched),
            static_cast<unsigned long long>(materialUvs.counts.unmatched),
            static_cast<unsigned long long>(materialUvs.counts.unchanged));
        trace(uvSummary);
        materialUvs.discard();
        customUvMismatched += materialUvs.counts.mismatched;
        customUvUnmatched += materialUvs.counts.unmatched;
        customUvUnchanged += materialUvs.counts.unchanged;
        {
            std::lock_guard lock(refreshMutex);
            pendingCamera.begin(game, level, GetCurrentThreadId());
            trace("camera_pending\r\n");
        }
    }
    static void __fastcall onStartFrame(void *game) {
        auto &state = *active().load(std::memory_order_acquire);
        static thread_local bool insideFrame{};
        if (insideFrame) { state.frame(game); return; }
        struct FrameScope {
            bool &inside;
            explicit FrameScope(bool &value) : inside(value) { inside = true; }
            ~FrameScope() { inside = false; }
        } scope(insideFrame);
        // No actor render call is on the stack here. Only a completed outer
        // original frame can satisfy the reload barrier; nested frames cannot.
        const auto before = state.platformCycles.load();
        state.refreshAtFrame(game);
        const bool refreshed = before != state.platformCycles.load();
        if (refreshed) state.trace("game_frame_after_refresh begin\r\n");
        state.frame(game);
        if (refreshed) state.trace("game_frame_after_refresh end\r\n");
        state.observeCompletedFrame(game);
    }
    static std::uint64_t __fastcall onProgressTick(void *controller) {
        auto &state = *active().load(std::memory_order_acquire);
        void *callback{}, *table{}, *invoke{}, *model{};
        if (state.enabled.load(std::memory_order_acquire) &&
            native_profile::read(controller, state.progressCallbackOffset, callback) && callback &&
            native_profile::read(callback, 0, table) &&
            native_profile::read(table, state.progressInvokeOffset, invoke) && invoke == state.eventTarget &&
            native_profile::read(controller, state.progressModelOffset, model)) {
            auto *client = state.clientFromModel(model);
            auto *game = client ? state.clientGame(client) : nullptr;
            if (game && state.localPlayer(client)) {
                if (!state.resourcesReady(game)) {
                    ++state.previousReloadWaitFrames;
                    if (!state.progressWaiting.exchange(true))
                        state.trace("native_apply_wait_previous_reload\r\n");
                    // Do not execute or destroy the callback. Its controller
                    // stays alive on the native progress screen and ticks again.
                    return 0;
                }
                if (state.progressWaiting.exchange(false))
                    state.trace("native_apply_previous_reload_ready\r\n");
            }
        }
        return state.progressTick(controller);
    }
    static void __fastcall onNavigateApply(void *model, void *function) {
        auto &state = *active().load(std::memory_order_acquire);
        if (state.enabled.load(std::memory_order_acquire)) {
            ++state.navigations;
            state.trace("native_reload_screen_navigation begin\r\n");
        }
        // Preserve the game's callback ownership, progress-screen ticks and
        // readiness polling. Never invoke/consume its apply callback inline.
        state.navigate(model, function);
        if (state.enabled.load(std::memory_order_acquire))
            state.trace("native_reload_screen_navigation end\r\n");
    }
    static bool exchangeFlag(void *self, std::size_t offset, unsigned char value,
                             unsigned char &previous) noexcept {
        __try {
            auto *flag = static_cast<unsigned char *>(self) + offset;
            if (*flag > 1)
                return false;
            previous = *flag;
            *flag = value;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static std::uint64_t __fastcall onExit(void *closure) {
        auto &state = *active().load(std::memory_order_acquire);
        void *owner{}, *model{}, *client{};
        if (state.enabled.load(std::memory_order_acquire)) {
            ++state.exits;
            if (native_profile::read(closure, state.closureOwnerOffset, owner) && owner &&
                native_profile::read(owner, state.settingsModelOffset, model))
                client = state.clientFromModel(model);
            if (!client)
                ++state.failures;
        }
        ApplyScope scope(client);
        if (client) state.trace("settings_exit begin\r\n");
        const auto result = state.exit(closure);
        if (client) state.trace("settings_exit end\r\n");
        return result;
    }
    static void __fastcall onApplyEvent(void *closure, void *model) {
        auto &state = *active().load(std::memory_order_acquire);
        void *client{};
        if (state.enabled.load(std::memory_order_acquire)) {
            ++state.applyEvents;
            client = state.clientFromModel(model);
            if (!client)
                ++state.failures;
            else if (state.localPlayer(client)) {
                ++state.inWorldEvents;
                ++state.liveApplications;
            }
        }
        // This event executes after navigation. Keep the same client eligible
        // throughout the game's queued stack update and reload notifications.
        ApplyScope scope(client);
        if (client) state.trace("apply_event begin\r\n");
        state.event(closure, model);
        if (client) state.trace("apply_event end\r\n");
    }
    static std::uint64_t __fastcall onCompose(void *self, void *full, void *global, void *level) {
        auto &state = *active().load(std::memory_order_acquire);
        const bool enabled = state.enabled.load(std::memory_order_acquire);
        if (enabled)
            ++state.compositions;
        unsigned char previous{};
        const auto client = applyingClient();
        const bool inWorld = enabled && client && state.localPlayer(client);
        const bool changed = inWorld && exchangeFlag(self, state.includeGlobalOffset, 1, previous);
        if (inWorld && !changed)
            ++state.failures;
        struct Restore {
            Binding &state;
            void *self;
            bool changed;
            unsigned char previous;
            ~Restore() {
                unsigned char ignored{};
                if (changed && !exchangeFlag(self, state.includeGlobalOffset, previous, ignored))
                    ++state.failures;
            }
        } restore{state, self, changed, previous};
        if (changed)
            ++state.globalIncluded;
        if (changed) {
            auto *game = state.clientGame(client);
            auto *clientLevel = state.clientLevel(client);
            if (game && clientLevel && state.gamePacks(game) == self) {
                std::lock_guard lock(state.refreshMutex);
                state.pendingCamera.clear();
                state.reloadBarrier.clear();
                state.pendingRefresh.begin(game, clientLevel, self, GetCurrentThreadId());
                state.refreshPending.store(true, std::memory_order_release);
                state.trace("compose_in_world begin\r\n");
            } else {
                ++state.failures;
            }
        }
        const auto result = state.compose(self, full, global, level);
        if (changed) state.trace("compose_in_world end\r\n");
        return result;
    }

    static bool writeFlag(void *self, std::size_t offset) noexcept {
        __try {
            auto *flag = static_cast<unsigned char *>(self) + offset;
            if (*flag > 1)
                return false;
            *flag = 1;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    // The game owns allocations made by its string getter. Never destroy these
    // with the DLL's CRT allocator, including large aligned MSVC allocations.
    std::string screen(void *client) {
        alignas(8) std::array<std::byte, 32> buffer{};
        screenName(client, buffer.data());
        const detail::OwnedMsvcStringLayout layout{stringSizeOffset,         stringCapacityOffset,
                                                   stringInlineBytes,        maximumStringBytes,
                                                   largeAllocationThreshold, alignmentOverhead};
        return detail::copyOwnedMsvcString(buffer, layout, deallocate,
                                           [](const void *address, void *&value) {
                                               return native_profile::read(address, 0, value);
                                           });
    }

    static void __fastcall onInitialize(void *self) {
        auto &state = *active().load(std::memory_order_acquire);
        if (state.enabled.load(std::memory_order_acquire)) {
            if (writeFlag(self, state.reloadOffset))
                ++state.opened;
            else {
                ++state.failures;
                state.enabled.store(false, std::memory_order_release);
            }
        }
        state.initialize(self);
    }
    static void *__fastcall onConstruct(void *self, void *model, bool global, bool reload,
                                        const void *initialPack, bool legacySync) {
        auto &state = *active().load(std::memory_order_acquire);
        if (state.enabled.load(std::memory_order_acquire))
            global = reload = true;
        // Forward the fifth and sixth arguments too: they live on the Windows
        // x64 stack and cannot be discarded when detouring this constructor.
        return state.construct(self, model, global, reload, initialPack, legacySync);
    }
    static bool __fastcall onPreGame(void *client) {
        auto &state = *active().load(std::memory_order_acquire);
        const bool original = state.preGame(client);
        if (state.enabled.load(std::memory_order_acquire) && applyingClient() == client &&
            state.localPlayer(client)) {
            if (!original)
                ++state.overrides;
            return true;
        }
        thread_local bool queryingScreen{};
        if (!state.enabled.load(std::memory_order_acquire) || queryingScreen)
            return original;
        struct QueryGuard {
            bool &value;
            explicit QueryGuard(bool &flag) : value(flag) {
                value = true;
            }
            ~QueryGuard() {
                value = false;
            }
        } guard{queryingScreen};
        try {
            const bool inWorld = state.localPlayer(client) != nullptr;
            const auto name = inWorld ? state.screen(client) : std::string{};
            std::lock_guard lock(state.policyMutex);
            if (!inWorld) {
                state.policies.erase(client);
                return original;
            }
            if (!state.policies.contains(client) && state.policies.size() >= 8)
                return original;
            const bool result = state.policies[client].preGame(original, name, inWorld);
            if (result && !original)
                ++state.overrides;
            return result;
        } catch (...) {
            ++state.failures;
            return original;
        }
    }
};

NativeResourcePacks::NativeResourcePacks() : binding_(new Binding) {}
NativeResourcePacks::~NativeResourcePacks() {
    binding_->enabled.store(false, std::memory_order_release);
    // Installed thunks and their trampolines have process lifetime. Retain the
    // binding after disabling so an in-flight game call can finish safely.
    if (!binding_->retained)
        delete binding_;
}
bool NativeResourcePacks::load(const RuntimeProfile &profile, std::string &error) {
    if (binding_->retained) {
        error = "resource pack hooks already retained; restart required";
        return false;
    }
    if (!profile.supports({Feature::ResourcePacks}, error))
        return false;
    try {
        auto next = std::make_unique<Binding>();
        const auto root = parseNativeProfile(profile.json());
        const auto &layout = root.at("layouts").at("ResourcePackManager");
        if (layout.at("adapter").get<std::string>() != "windows-live-reload-v19-native-progress")
            throw std::runtime_error("unsupported resource pack adapter ABI");
        next->materialUvs.load(root, layout);
        const auto get = [&](const char *field, const char *abi) {
            const auto &definition = layout.at("functions").at(field);
            if (definition.at("abi").get<std::string>() != abi)
                throw std::runtime_error(std::string("resource pack ABI mismatch: ") + field);
            return native_profile::checked(root, definition);
        };
        next->initTarget = get("initialize_settings", "void __fastcall(SettingsScreenController*)");
        next->ctorTarget = get("construct_general_settings",
                               "void* __fastcall(GeneralSettingsScreenController*, void* model, "
                               "bool, bool, const void* initialPack, bool)");
        next->preGameTarget = get("is_pre_game", "bool __fastcall(ClientInstance*)");
        next->clientGame = reinterpret_cast<Binding::ClientGame>(
            get("client_game", "void* __fastcall(ClientInstance*)"));
        next->gameInGameTarget = get("game_is_in_game", "bool __fastcall(MinecraftGame*)");
        // Check both the listener entry and the exact return-address guard.
        get("on_full_stack_invalid", "bool __fastcall(ResourcePackListener*)");
        next->gameGateCaller = native_profile::checked(root, layout.at("game_in_game_caller"));
        next->navigateTarget = get("navigate_apply_packs",
                                    "void __fastcall(MainMenuScreenModel*, void* callback)");
        next->progressTickTarget = get("reload_progress_tick", "uint64_t __fastcall(ResourcePackProgressScreenController*)");
        const auto &progress = layout.at("reload_progress_gate");
        next->progressModelOffset = native_profile::number(progress.at("model_offset"));
        next->progressCallbackOffset = native_profile::number(progress.at("callback_owner_offset"));
        next->progressInvokeOffset = native_profile::number(progress.at("callback_invoke_slot_offset"));
        for (auto offset : {next->progressModelOffset, next->progressCallbackOffset, next->progressInvokeOffset})
            if (!offset || offset > 0x10000 || offset % sizeof(void *))
                throw std::runtime_error("invalid resource reload progress layout");
        next->exitTarget = get("exit_settings", "uint64_t __fastcall(void* closure)");
        next->eventTarget = get("apply_stack_event", "void __fastcall(void* closure, void* model)");
        next->composeTarget = get("compose_full_stack",
                                   "uint64_t __fastcall(ResourcePackManager*, void*, void*, void*)");
        next->closureOwnerOffset = native_profile::number(layout.at("exit_closure_owner_offset"));
        next->settingsModelOffset = native_profile::number(layout.at("settings_model_offset"));
        next->modelClientOffset = native_profile::number(layout.at("model_client_offset"));
        next->includeGlobalOffset = native_profile::number(layout.at("include_global_stack_offset"));
        for (auto offset : {next->closureOwnerOffset, next->settingsModelOffset,
                            next->modelClientOffset, next->includeGlobalOffset})
            if (offset < 8 || offset > 0x10000)
                throw std::runtime_error("invalid resource pack apply layout");
        next->screenName = reinterpret_cast<Binding::ScreenName>(
            get("top_screen_name", "void* __fastcall(ClientInstance*, void* stringResult)"));
        next->localPlayer = reinterpret_cast<Binding::LocalPlayer>(get("local_player", "LocalPlayer* __fastcall(ClientInstance*)"));
        next->frameTarget = get("start_frame", "void __fastcall(MinecraftGame*)");
        next->cameraFrameTarget = get("camera_frame", "void __fastcall(LevelRendererPlayer*, void* frameContext)");
        next->cameraRebind = reinterpret_cast<Binding::Event>(get("camera_rebind", "void __fastcall(LevelRendererCamera*, void* player)"));
        next->blockGraphicsTarget = get("get_block_graphics", "void* __fastcall(const void* block)");
        next->cachedGraphicsTarget = get("read_cached_graphics", "void __fastcall(void* capture, void* clientData)");
        next->initializeBlockGraphics = reinterpret_cast<Binding::Event>(get("initialize_block_graphics", "void __fastcall(ResourcePackManager*, const void* experiments)"));
        next->remoteClient = reinterpret_cast<Binding::Predicate>(get("remote_client", "bool __fastcall(ClientInstance*)"));
        next->gamePreSuspendTarget = get("game_pre_suspend", "void __fastcall(AppPlatformListener*)");
        next->gameSuspendTarget = get("game_suspend", "void __fastcall(AppPlatformListener*)");
        next->gameResumeTarget = get("game_resume", "void __fastcall(AppPlatformListener*)");
        next->suspendGateTarget = get("suspend_gate_candidate", "bool __fastcall(MinecraftGame*)");
        next->suspendGateCaller1 = native_profile::checked(root, layout.at("suspend_gate_caller1"));
        next->suspendGateCaller2 = native_profile::checked(root, layout.at("suspend_gate_caller2"));
        next->platformSuspend = reinterpret_cast<Binding::Initialize>(get("platform_suspend", "void __fastcall(AppPlatform*)"));
        next->platformResume = reinterpret_cast<Binding::Initialize>(get("platform_resume", "void __fastcall(AppPlatform*)"));
        next->clientRenderer = reinterpret_cast<Binding::ClientGame>(get("client_renderer", "void* __fastcall(ClientInstance*)"));
        next->drainRendererTasks = reinterpret_cast<Binding::Initialize>(get("drain_renderer_tasks", "void __fastcall(TaskGroup*)"));
        next->geometryReload = reinterpret_cast<Binding::GeometryReload>(get("reload_geometry", "void __fastcall(MinecraftGame*, bool sync)"));
        next->notifyClientResources = reinterpret_cast<Binding::Event>(get("notify_client_resources", "void __fastcall(void* gameClosure, ClientInstance*)"));
        const auto &renderer = layout.at("level_renderer");
        next->rendererTasksOffset = native_profile::number(renderer.at("task_group_offset"));
        next->rendererCameraOffset = native_profile::number(renderer.at("player_camera_offset"));
        const auto &camera = layout.at("player_camera");
        next->cameraOwnerOffset = native_profile::number(camera.at("renderer_offset"));
        next->cameraDimensionOffset = native_profile::number(camera.at("dimension_offset"));
        next->playerDimensionOffset = native_profile::number(camera.at("player_dimension_offset"));
        next->playerDimensionControlOffset = native_profile::number(camera.at("player_dimension_control_offset"));
        next->cameraFrameSlot = native_profile::number(camera.at("frame_slot_offset"));
        next->cachedGraphicsOffset = native_profile::number(layout.at("block_client_graphics_offset"));
        const auto &gameListener = layout.at("game_lifecycle_listener");
        next->gameListenerOffset = native_profile::number(gameListener.at("game_offset"));
        next->gamePreSlot = native_profile::number(gameListener.at("pre_suspend_slot"));
        next->gameSuspendSlot = native_profile::number(gameListener.at("suspend_slot"));
        next->gameResumeSlot = native_profile::number(gameListener.at("resume_slot"));
        for (auto offset : {next->gameListenerOffset, next->gamePreSlot, next->gameSuspendSlot, next->gameResumeSlot})
            if (offset > 0x10000 || offset % sizeof(void *)) throw std::runtime_error("invalid game lifecycle layout");
        const auto &platform = layout.at("app_platform");
        const auto platformRva = native_profile::number(platform.at("global_rva"));
        if (platformRva >= native_profile::number(root.at("target").at("size_of_image")) - 24)
            throw std::runtime_error("invalid app platform global");
        native_profile::checked(root, platform.at("global_store_guard"));
        next->platformGlobal = reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) + platformRva);
        next->platformObjectOffset = native_profile::number(platform.at("object_offset"));
        next->platformSuspendSlot = native_profile::number(platform.at("suspend_slot"));
        next->platformResumeSlot = native_profile::number(platform.at("resume_slot"));
        for (auto offset : {next->rendererTasksOffset, next->rendererCameraOffset, next->cameraOwnerOffset,
                            next->cameraDimensionOffset, next->playerDimensionOffset, next->playerDimensionControlOffset, next->cameraFrameSlot, next->cachedGraphicsOffset,
                            next->platformObjectOffset, next->platformSuspendSlot, next->platformResumeSlot})
            if (offset > 0x10000 || offset % sizeof(void *)) throw std::runtime_error("invalid renderer or platform layout");
        native_profile::checked(root, layout.at("primary_client_lookup_entry"));
        const auto &clients = layout.at("primary_clients");
        next->clientsOffset = native_profile::number(clients.at("map_offset"));
        next->nodeLeftOffset = native_profile::number(clients.at("left_offset"));
        next->nodeNilOffset = native_profile::number(clients.at("nil_offset"));
        next->nodeKeyOffset = native_profile::number(clients.at("key_offset"));
        next->nodeClientOffset = native_profile::number(clients.at("client_offset"));
        const auto &loader = layout.at("resource_loader");
        next->loaderOffset = native_profile::number(loader.at("game_offset"));
        next->loaderReadyOffset = native_profile::number(loader.at("ready_offset"));
        next->loaderOutstandingOffset = native_profile::number(loader.at("outstanding_offset"));
        for (auto offset : {next->clientsOffset, next->nodeLeftOffset, next->nodeNilOffset,
                            next->nodeKeyOffset, next->nodeClientOffset, next->loaderOffset,
                            next->loaderReadyOffset, next->loaderOutstandingOffset})
            if (offset > 0x10000)
                throw std::runtime_error("invalid frame reload layout");
        next->clientLevel = reinterpret_cast<Binding::ClientGame>(get("client_level", "void* __fastcall(ClientInstance*)"));
        next->levelData = reinterpret_cast<Binding::ClientGame>(get("level_data", "void* __fastcall(Level*)"));
        next->gamePacks = reinterpret_cast<Binding::ClientGame>(get("game_packs", "void* __fastcall(MinecraftGame*)"));
        next->atlasTarget = get("update_atlas_pbr", "void __fastcall(MinecraftGame*, const void* atlas, bool terrain)");
        next->atlasCaller = native_profile::checked(root, layout.at("terrain_completion_caller"));
        next->experimentsOffset = native_profile::number(layout.at("level_data_experiments_offset"));
        next->terrainAtlasOffset = native_profile::number(layout.at("terrain_atlas_offset"));
        if (!next->experimentsOffset || next->experimentsOffset > 0x10000 ||
            !next->terrainAtlasOffset || next->terrainAtlasOffset > 0x10000)
            throw std::runtime_error("invalid terrain refresh layout");
        next->deallocate =
            reinterpret_cast<Binding::Deallocate>(get("deallocate", "void __fastcall(void*)"));
        next->reloadOffset = native_profile::number(layout.at("reload_on_exit_offset"));
        const auto &string = layout.at("string_abi");
        next->stringObjectBytes = native_profile::number(string.at("object_bytes"));
        next->stringSizeOffset = native_profile::number(string.at("size_offset"));
        next->stringCapacityOffset = native_profile::number(string.at("capacity_offset"));
        next->stringInlineBytes = native_profile::number(string.at("inline_bytes"));
        next->maximumStringBytes = native_profile::number(string.at("maximum_bytes"));
        next->largeAllocationThreshold =
            native_profile::number(string.at("large_allocation_threshold"));
        next->alignmentOverhead = native_profile::number(string.at("alignment_overhead"));
        if (next->reloadOffset < 32 || next->reloadOffset > 0x10000 ||
            next->stringObjectBytes != 32 || next->stringSizeOffset != 16 ||
            next->stringCapacityOffset != 24 || next->stringInlineBytes != 16 ||
            next->maximumStringBytes == 0 || next->maximumStringBytes > 4096 ||
            next->largeAllocationThreshold != 4096 || next->alignmentOverhead != 39)
            throw std::runtime_error("unsupported resource pack layout or MSVC string ABI");
        delete binding_;
        binding_ = next.release();
        error.clear();
        return true;
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}
bool NativeResourcePacks::install(std::string &error) {
    std::lock_guard lock(Binding::installationMutex());
    if (binding_->installed.load()) {
        error.clear();
        return true;
    }
    if (!binding_->initTarget || !binding_->ctorTarget || !binding_->preGameTarget ||
        !binding_->screenName || !binding_->localPlayer || !binding_->deallocate ||
        !binding_->exitTarget || !binding_->eventTarget || !binding_->composeTarget ||
        !binding_->navigateTarget || !binding_->progressTickTarget ||
        !binding_->clientGame || !binding_->gameInGameTarget || !binding_->gameGateCaller ||
        !binding_->atlasTarget || !binding_->atlasCaller || !binding_->clientLevel ||
        !binding_->frameTarget || !binding_->cameraFrameTarget || !binding_->cameraRebind ||
        !binding_->blockGraphicsTarget || !binding_->cachedGraphicsTarget || !binding_->initializeBlockGraphics ||
        !binding_->platformGlobal || !binding_->platformSuspend || !binding_->platformResume ||
        !binding_->remoteClient || !binding_->gamePreSuspendTarget || !binding_->gameSuspendTarget || !binding_->gameResumeTarget ||
        !binding_->suspendGateTarget || !binding_->suspendGateCaller1 || !binding_->suspendGateCaller2 ||
        !binding_->clientRenderer || !binding_->drainRendererTasks || !binding_->levelData || !binding_->gamePacks ||
        !binding_->geometryReload || !binding_->notifyClientResources) {
        error = "resource pack profile has not been loaded";
        return false;
    }
    if (Binding::active().load(std::memory_order_acquire)) {
        error = "resource pack adapter already owned";
        return false;
    }
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&Binding::onInitialize), &pinned)) {
        error = "could not pin resource pack hook module";
        return false;
    }
    binding_->retained = true;
    Binding::active().store(binding_, std::memory_order_release);
    const bool installed =
        BDS_HookDirectInstall(
            binding_->initTarget, reinterpret_cast<void *>(&Binding::onInitialize),
            reinterpret_cast<void **>(&binding_->initialize), &binding_->tokens[0]) &&
        BDS_HookDirectInstall(binding_->ctorTarget, reinterpret_cast<void *>(&Binding::onConstruct),
                              reinterpret_cast<void **>(&binding_->construct),
                              &binding_->tokens[1]) &&
        BDS_HookDirectInstall(binding_->preGameTarget,
                              reinterpret_cast<void *>(&Binding::onPreGame),
                              reinterpret_cast<void **>(&binding_->preGame), &binding_->tokens[2]) &&
        BDS_HookDirectInstall(binding_->exitTarget, reinterpret_cast<void *>(&Binding::onExit),
                              reinterpret_cast<void **>(&binding_->exit), &binding_->tokens[3]) &&
        BDS_HookDirectInstall(binding_->eventTarget, reinterpret_cast<void *>(&Binding::onApplyEvent),
                              reinterpret_cast<void **>(&binding_->event), &binding_->tokens[4]) &&
        BDS_HookDirectInstall(binding_->composeTarget, reinterpret_cast<void *>(&Binding::onCompose),
                              reinterpret_cast<void **>(&binding_->compose), &binding_->tokens[5]) &&
        BDS_HookDirectInstall(binding_->navigateTarget,
                              reinterpret_cast<void *>(&Binding::onNavigateApply),
                              reinterpret_cast<void **>(&binding_->navigate), &binding_->tokens[6]) &&
        BDS_HookDirectInstall(binding_->gameInGameTarget,
                              reinterpret_cast<void *>(&Binding::onGameInGame),
                              reinterpret_cast<void **>(&binding_->gameInGame), &binding_->tokens[7]) &&
        BDS_HookDirectInstall(binding_->atlasTarget, reinterpret_cast<void *>(&Binding::onAtlasUpdate),
                              reinterpret_cast<void **>(&binding_->atlasUpdate), &binding_->tokens[8]) &&
        BDS_HookDirectInstall(binding_->frameTarget, reinterpret_cast<void *>(&Binding::onStartFrame),
                              reinterpret_cast<void **>(&binding_->frame), &binding_->tokens[9]) &&
        BDS_HookDirectInstall(binding_->cameraFrameTarget, reinterpret_cast<void *>(&Binding::onCameraFrame),
                              reinterpret_cast<void **>(&binding_->cameraFrame), &binding_->tokens[10]) &&
        BDS_HookDirectInstall(binding_->blockGraphicsTarget, reinterpret_cast<void *>(&Binding::onBlockGraphics),
                              reinterpret_cast<void **>(&binding_->blockGraphics), &binding_->tokens[11]) &&
        BDS_HookDirectInstall(binding_->cachedGraphicsTarget, reinterpret_cast<void *>(&Binding::onCachedGraphics),
                              reinterpret_cast<void **>(&binding_->cachedGraphics), &binding_->tokens[12]) &&
        BDS_HookDirectInstall(binding_->suspendGateTarget, reinterpret_cast<void *>(&Binding::onSuspendGate),
                              reinterpret_cast<void **>(&binding_->suspendGate), &binding_->tokens[13]) &&
        BDS_HookDirectInstall(binding_->progressTickTarget, reinterpret_cast<void *>(&Binding::onProgressTick),
                              reinterpret_cast<void **>(&binding_->progressTick), &binding_->tokens[14]);
    if (!installed) {
        // The broker's direct trampolines have process lifetime. Keep any
        // successfully installed thunk disabled and forwarding to the game;
        // removing its trampoline could race a game call during startup.
        error = "resource pack hook registration conflict; restart Minecraft before retrying";
        return false;
    }
    binding_->installed.store(true);
    binding_->enabled.store(true, std::memory_order_release);
    error.clear();
    return true;
}
void NativeResourcePacks::setTraceCallback(TraceCallback callback) noexcept {
    binding_->traceCallback.store(callback, std::memory_order_release);
}
NativeResourcePacks::Status NativeResourcePacks::status() const noexcept {
    return {binding_->installed.load(), binding_->opened.load(), binding_->overrides.load(),
            binding_->failures.load(), binding_->exits.load(), binding_->applyEvents.load(),
            binding_->compositions.load(),
            binding_->globalIncluded.load(), binding_->navigations.load(),
            binding_->liveApplications.load(), binding_->inWorldEvents.load(), binding_->gameGateOverrides.load(),
            binding_->terrainCompleted.load(), binding_->refreshCancelled.load(), binding_->resourceWaitFrames.load(),
            binding_->platformCycles.load(), binding_->blockGraphicsRebuilt.load(),
            binding_->graphicsCacheResets.load(), binding_->camerasRefreshed.load(),
            binding_->remoteReloads.load(), binding_->suspendGateOverrides.load(),
            binding_->geometryReloaded.load(), binding_->clientDefinitionsReloaded.load(),
            binding_->customMaterialsRemapped.load(), binding_->customUvSetsUpdated.load(),
            binding_->customUvMismatched.load(), binding_->customUvUnmatched.load(), binding_->customUvUnchanged.load(), binding_->nativeScreenWaitFrames.load(), binding_->previousReloadWaitFrames.load()};
}
} // namespace bedrock
