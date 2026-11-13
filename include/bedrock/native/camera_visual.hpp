#pragma once
#include <bedrock/api/geometry.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/detail/native_world_access.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <nlohmann/json.hpp>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace bedrock {
// Value copy of one throttled camera-visual observation. Native reads happen
// on the owning render callbacks; workers only ever see this copy.
struct CameraVisibilitySample {
    std::uint64_t epoch{}, milliseconds{};
    int gameType = -1;
    bool flying{}, valid{};
    Vec3 cameraPosition{}, origin{};
    std::array<unsigned char, 7> rawFlags{};
    unsigned char cullerType{};
    std::uintptr_t cullerVtable{};
    bool viewValid{};
    std::array<unsigned char, 13> viewFlags{};
    std::optional<bool> drawInsideCubes{};
};

// Counter fields are exchanged by takeStatistics(); the rest are loads.
struct CameraVisualStatistics {
    unsigned cameraRenderCalls{}, viewCreated{}, viewChanged{}, viewWriteFailed{};
    unsigned terrainCalls{}, terrainChanges{}, terrainRestores{}, terrainFaults{};
    unsigned selectorRequests{}, selectorRewrites{};
    std::uintptr_t selectorCaller{};
    int terrainObservedMode = -1;
    bool terrainPending{};
    bool terrainRejected{};
};

// Camera visual paths whose native layout lives in the profile's
// native_camera.visual definition: terrain culler selection (selector hook,
// lease and switch budget), the read-only camera-render observer, the view
// factory output fix-up and the visibility diagnostics sampler. All RVA /
// entry-guard resolution, offsets and vtable identity checks are read from
// the selected profile here; the host Mod supplies only policy callbacks and
// consumes value samples and statistics. Hooks stay pinned for the module's
// lifetime and pass through whenever the policy is false, matching the
// lifetime of the shared hook runtime.
class NativeCameraVisual {
public:
    struct Policy {
        bool (*active)() noexcept = nullptr;
        bool (*stopped)() noexcept = nullptr;
        bool (*spectatorMode)() noexcept = nullptr;
        bool (*diagnosticsEnabled)() noexcept = nullptr;
        std::uint64_t (*epoch)() noexcept = nullptr;
        std::uint64_t (*flightSerial)() noexcept = nullptr;
    };

    // Parses the visual layout out of an already profile-manager-selected,
    // parseNativeProfile-expanded root. rendererPlayerOffset / renderOrigin
    // come from the caller's WorldRenderProfile. Returns false with `error`
    // when any required definition is missing; every install then fails too.
    bool load(const nlohmann::json &root, std::uint32_t rendererPlayerOffset,
              std::uint32_t renderOriginOffset, Policy policy, std::string &error) {
        ready_ = false;
        try {
            if (!policy.active || !policy.stopped || !policy.spectatorMode ||
                !policy.diagnosticsEnabled || !policy.epoch || !policy.flightSerial)
                throw std::runtime_error("camera visual policy is incomplete");
            policy_ = policy;
            root_ = root;
            rendererPlayerOffset_ = rendererPlayerOffset;
            renderOriginOffset_ = renderOriginOffset;
            const auto &visual = root.at("native_camera").at("visual");
            behavior_ = visual.value("spectator_mode_behavior", std::string{});
            const auto &culling = visual.at("terrain_culling");
            terrainSchema_ = native_profile::number(culling.at("schema"));
            spectatorMode_ = static_cast<unsigned char>(native_profile::number(culling.at("spectator_mode")));
            setCullerDef_ = culling.at("set_culler");
            factoryCheckDef_ = culling.at("factory_check");
            spectatorUpdateDef_ = culling.at("spectator_update_check");
            cullerOffset_ = native_profile::number(culling.at("camera_culler_offset"));
            cullerTypeOffset_ = native_profile::number(culling.at("camera_culler_type_offset"));
            // Null while the terrain path is unresolved for a build; only
            // installTerrain() may reject on it, after the behavior gate.
            spectatorVtableRva_ = culling.at("spectator_vtable_rva").is_null()
                                      ? 0
                                      : native_profile::number(culling.at("spectator_vtable_rva"));
            const auto &observe = visual.at("camera_render_observe");
            observeDef_ = observe;
            viewFlagsOffset_ = native_profile::number(observe.at("view_flags_offset"));
            viewFlagsCount_ = native_profile::number(observe.at("view_flags_count"));
            if (viewFlagsCount_ > 13)
                throw std::runtime_error("unsupported camera view flags count");
            insideCubesOffset_ = native_profile::number(observe.at("draw_inside_cubes_offset"));
            const auto &createView = visual.at("create_view");
            createViewDef_ = createView;
            if (createView.contains("output_draw_inside_cubes_offset"))
                insideCubesOffset_ =
                    native_profile::number(createView.at("output_draw_inside_cubes_offset"));
            if (insideCubesOffset_ < viewFlagsOffset_ ||
                insideCubesOffset_ - viewFlagsOffset_ >= viewFlagsCount_)
                throw std::runtime_error("camera view inside-cubes flag outside the flags range");
            insideCubesIndex_ = insideCubesOffset_ - viewFlagsOffset_;
            // Diagnostics definitions postdate the other visual paths; a
            // profile without them keeps the hooks and only loses sampling.
            if (visual.contains("diagnostics")) {
                const auto &diagnostics = visual.at("diagnostics");
                cameraPositionOffset_ =
                    native_profile::number(diagnostics.at("camera_position_candidate_offset"));
                rawFlagsOffset_ = native_profile::number(diagnostics.at("raw_flags_offset"));
                if (native_profile::number(diagnostics.at("raw_flags_size")) != 7)
                    throw std::runtime_error("unsupported camera diagnostics flags size");
                diagnosticsReady_ = true;
            }
            ready_ = true;
            return true;
        } catch (const std::exception &e) {
            error = e.what();
            return false;
        }
    }

    // Mirrors the historical Freecam startup gating: the terrain path only
    // arms when the profile carries the mode5 behavior as established, the
    // exact-build renderer layout resolved, and the profile's own vtable
    // identity check agrees with its spectator-update function.
    bool installTerrain(bool rendererReady, std::string &error) {
        try {
            if (!ready_)
                throw std::runtime_error("camera visual layout unavailable");
            if (behavior_ != "native_spectator_culler_mode5")
                throw std::runtime_error(
                    "spectator terrain profile is unresolved; basic Freecam remains available");
            if (!rendererReady)
                throw std::runtime_error("exact-build renderer layout unavailable");
            if (terrainSchema_ != 1 || spectatorMode_ != 5)
                throw std::runtime_error("unsupported terrain culler schema");
            auto *target = native_profile::checked(root_, setCullerDef_);
            native_profile::checked(root_, factoryCheckDef_);
            void *updateSpectator = native_profile::checked(root_, spectatorUpdateDef_);
            if (!spectatorVtableRva_)
                throw std::runtime_error("terrain culler identity mismatch");
            const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            spectatorVtable_ = base + spectatorVtableRva_;
            void *updateEntry{};
            if (!world_native::read(reinterpret_cast<const void *>(spectatorVtable_), 8,
                                    updateEntry) ||
                updateEntry != updateSpectator)
                throw std::runtime_error("terrain culler identity mismatch");
            State &s = state();
            if (s.cullerOwner)
                throw std::runtime_error("camera visual culler hook already installed");
            s.cullerOwner = this;
            if (!BDS_HookDirectInstall(target, reinterpret_cast<void *>(&cullerDetour),
                                       reinterpret_cast<void **>(&originalSetCuller_),
                                       &cullerToken_)) {
                s.cullerOwner = nullptr;
                throw std::runtime_error("native culler selector hook conflict");
            }
            setCuller_ = originalSetCuller_;
            terrainReady_.store(true);
            return true;
        } catch (const std::exception &e) {
            error = e.what();
            return false;
        }
    }

    bool installRenderObserve(bool rendererReady, std::string &error) {
        try {
            if (!ready_)
                throw std::runtime_error("camera visual layout unavailable");
            if (!rendererReady)
                throw std::runtime_error("exact-build renderer layout unavailable");
            void *target = native_profile::checked(root_, observeDef_);
            State &s = state();
            if (s.observeOwner)
                throw std::runtime_error("camera visual observer already installed");
            s.observeOwner = this;
            if (!BDS_HookDirectInstall(target, reinterpret_cast<void *>(&cameraRenderDetour),
                                       reinterpret_cast<void **>(&originalCameraRender_),
                                       &cameraRenderToken_)) {
                s.observeOwner = nullptr;
                error = "hook conflict";
                return false;
            }
            observeReady_.store(true);
            return true;
        } catch (const std::exception &e) {
            error = e.what();
            return false;
        }
    }

    bool installCreateView(std::string &error) {
        try {
            if (!ready_)
                throw std::runtime_error("camera visual layout unavailable");
            void *target = native_profile::checked(root_, createViewDef_);
            State &s = state();
            if (s.createViewOwner)
                throw std::runtime_error("camera visual view factory already installed");
            s.createViewOwner = this;
            if (!BDS_HookDirectInstall(target, reinterpret_cast<void *>(&createViewDetour),
                                       reinterpret_cast<void **>(&originalCreateView_),
                                       &createViewToken_)) {
                s.createViewOwner = nullptr;
                error = "hook conflict";
                return false;
            }
            createViewReady_.store(true);
            return true;
        } catch (const std::exception &e) {
            error = e.what();
            return false;
        }
    }

    bool terrainReady() const noexcept { return terrainReady_.load(std::memory_order_relaxed); }
    bool observeReady() const noexcept { return observeReady_.load(std::memory_order_relaxed); }
    bool createViewReady() const noexcept { return createViewReady_.load(std::memory_order_relaxed); }
    bool leaseActive() const noexcept { return terrainLeaseActive_.load(std::memory_order_relaxed); }

    void notePrimaryCamera(void *camera) noexcept {
        primaryCamera_.store(camera);
        primaryCameraEpoch_.store(policy_.epoch ? policy_.epoch() : 0);
    }
    std::uint64_t primaryCameraEpoch() const noexcept {
        return primaryCameraEpoch_.load(std::memory_order_relaxed);
    }
    void noteNativeModeContext(std::uint64_t context) noexcept {
        nativeModeContext_.store(context);
    }
    void setDiagnosticGameType(int type, std::uint64_t epoch, std::uint64_t milliseconds) noexcept {
        diagnosticGameType_.store(type);
        diagnosticGameTime_.store(milliseconds);
        diagnosticGameEpoch_.store(epoch);
    }

    // Per-frame terrain maintenance, called from the render subscriber with
    // the LevelRenderer. Ported lease semantics: a native game-mode change
    // surrenders ownership, another caller's mode is never overwritten, and a
    // busy asynchronous culler is retried on a later frame.
    void updateTerrain(void *renderer) {
        if (!terrainReady_.load(std::memory_order_relaxed))
            return;
        std::unique_lock leaseLock(terrainLeaseMutex_, std::try_to_lock);
        if (!leaseLock)
            return;
        resetCullerBudgetLocked();
        const auto world = policy_.epoch();
        void *camera{};
        unsigned char mode{};
        if (cullerLease_.epoch != world) {
            cullerLease_ = {};
            terrainLeaseActive_.store(false);
        }
        if (!world_native::read(renderer, rendererPlayerOffset_, camera) || !camera ||
            !world_native::read(camera, cullerTypeOffset_, mode) || mode >= kCullerModeCount)
            return;
        if (cullerLease_.camera && cullerLease_.camera != camera) {
            cullerLease_ = {};
            terrainLeaseActive_.store(false);
        }
        const auto nativeContext = nativeModeContext_.load();
        // A native game-mode change surrenders ownership to the engine. In
        // particular, never restore mode3 over a newly entered real spectator.
        if (cullerLease_.owned && nativeContext && nativeContext != cullerLease_.nativeModeContext) {
            cullerLease_ = {};
            terrainLeaseActive_.store(false);
        }
        terrainObservedMode_.store(mode, std::memory_order_relaxed);
        const bool want = policy_.active() && !policy_.stopped() && policy_.spectatorMode() &&
                          !terrainRejected_.load();
        // Native setter checks that its asynchronous culler is idle, constructs
        // the replacement and resets visibility caches. A busy culler is retried
        // on a later frame; no spin/wait and no manual native shared_ptr swap.
        if (want) {
            if (mode != kNativeDefaultCullerMode && mode != spectatorMode_) {
                terrainPending_.store(true);
                return;
            }
            if (mode == spectatorMode_) {
                void *culler{};
                std::uintptr_t table{};
                const bool expected =
                    world_native::read(camera, cullerOffset_, culler) &&
                    world_native::read(culler, 0, table) && table == spectatorVtable_;
                terrainPending_.store(!expected, std::memory_order_relaxed);
                return;
            }
            if ((nativeContext >> 8) != world) {
                terrainPending_.store(true);
                return;
            }
            if (!cullerLease_.owned) {
                cullerLease_ = {camera, world, mode, true, nativeContext};
                terrainLeaseActive_.store(true);
            }
            if (policy_.epoch() != world)
                return;
            terrainCalls_.fetch_add(1, std::memory_order_relaxed);
            if (!invokeCuller(camera, spectatorMode_)) {
                terrainFaults_.fetch_add(1);
                terrainReady_.store(false);
                return;
            }
            unsigned char after{};
            if (world_native::read(camera, cullerTypeOffset_, after)) {
                terrainObservedMode_.store(after, std::memory_order_relaxed);
                if (after == spectatorMode_)
                    recordCullerSwitchLocked();
                terrainPending_.store(after != spectatorMode_, std::memory_order_relaxed);
            }
        } else if (cullerLease_.owned) {
            // A different mode means another native caller has already changed it;
            // do not overwrite that caller's choice with a stale saved value.
            if (mode != spectatorMode_) {
                if (mode == cullerLease_.previous)
                    terrainRestores_.fetch_add(1);
                cullerLease_ = {};
                terrainLeaseActive_.store(false);
                terrainPending_.store(false);
                return;
            }
            if (policy_.epoch() != world)
                return;
            if (!invokeCuller(camera, cullerLease_.previous)) {
                terrainFaults_.fetch_add(1);
                terrainReady_.store(false);
                return;
            }
            unsigned char after{};
            if (world_native::read(camera, cullerTypeOffset_, after)) {
                terrainObservedMode_.store(after, std::memory_order_relaxed);
                if (after == cullerLease_.previous) {
                    cullerLease_ = {};
                    terrainLeaseActive_.store(false);
                    terrainRestores_.fetch_add(1);
                    terrainPending_.store(false);
                } else
                    terrainPending_.store(true);
            }
        } else
            terrainPending_.store(false, std::memory_order_relaxed);
    }

    // Render-subscriber sampling path, used when the observe hook is not
    // installed. No-op unless diagnostics are enabled in the host policy.
    void captureFromRenderer(void *renderer) { capture(renderer, nullptr, false); }

    std::optional<CameraVisibilitySample> takeSample() {
        std::unique_lock lock(sampleMutex_, std::try_to_lock);
        if (!lock || !sampleReady_)
            return std::nullopt;
        sampleReady_ = false;
        return sample_;
    }

    CameraVisualStatistics statistics() const noexcept {
        CameraVisualStatistics s;
        s.cameraRenderCalls = cameraRenderCalls_.load(std::memory_order_relaxed);
        s.viewCreated = viewCreated_.load(std::memory_order_relaxed);
        s.viewChanged = viewChanged_.load(std::memory_order_relaxed);
        s.viewWriteFailed = viewWriteFailed_.load(std::memory_order_relaxed);
        s.terrainCalls = terrainCalls_.load(std::memory_order_relaxed);
        s.terrainChanges = terrainChanges_.load(std::memory_order_relaxed);
        s.terrainRestores = terrainRestores_.load(std::memory_order_relaxed);
        s.terrainFaults = terrainFaults_.load(std::memory_order_relaxed);
        s.selectorRequests = selectorRequests_.load(std::memory_order_relaxed);
        s.selectorRewrites = selectorRewrites_.load(std::memory_order_relaxed);
        s.selectorCaller = selectorCaller_.load(std::memory_order_relaxed);
        s.terrainObservedMode = terrainObservedMode_.load(std::memory_order_relaxed);
        s.terrainPending = terrainPending_.load(std::memory_order_relaxed);
        s.terrainRejected = terrainRejected_.load(std::memory_order_relaxed);
        return s;
    }
    CameraVisualStatistics takeStatistics() noexcept {
        CameraVisualStatistics s = statistics();
        s.cameraRenderCalls = cameraRenderCalls_.exchange(0, std::memory_order_relaxed);
        s.viewCreated = viewCreated_.exchange(0, std::memory_order_relaxed);
        s.viewChanged = viewChanged_.exchange(0, std::memory_order_relaxed);
        s.viewWriteFailed = viewWriteFailed_.exchange(0, std::memory_order_relaxed);
        s.terrainCalls = terrainCalls_.exchange(0, std::memory_order_relaxed);
        s.terrainChanges = terrainChanges_.exchange(0, std::memory_order_relaxed);
        s.terrainRestores = terrainRestores_.exchange(0, std::memory_order_relaxed);
        s.terrainFaults = terrainFaults_.exchange(0, std::memory_order_relaxed);
        s.selectorRequests = selectorRequests_.exchange(0, std::memory_order_relaxed);
        s.selectorRewrites = selectorRewrites_.exchange(0, std::memory_order_relaxed);
        return s;
    }

private:
    using SetCullerFn = void(__fastcall *)(void *, unsigned char);
    using CameraRenderFn = void(__fastcall *)(void *, void *, void *, void *);
    using CreateViewFn = void *(__fastcall *)(void *, void *, void *, unsigned char);
    // The game's ordinary (non-spectator) culler type the selector is asked
    // for during normal play, and the number of culler types the engine has.
    static constexpr unsigned char kNativeDefaultCullerMode = 3;
    static constexpr unsigned char kCullerModeCount = 6;

    struct State {
        NativeCameraVisual *cullerOwner{}, *observeOwner{}, *createViewOwner{};
    };
    static State &state() {
        static State s;
        return s;
    }

    static bool finite(Vec3 v) noexcept {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
               std::abs(v.x) < 40000000 && std::abs(v.y) < 40000000 && std::abs(v.z) < 40000000;
    }

    void resetCullerBudgetLocked() {
        const auto serial = policy_.flightSerial();
        if (serial != budgetFlight_) {
            budgetFlight_ = serial;
            budgetSwitches_ = 0;
            terrainRejected_.store(false);
        }
    }
    void recordCullerSwitchLocked() {
        terrainChanges_.fetch_add(1, std::memory_order_relaxed);
        if (++budgetSwitches_ >= 8)
            terrainRejected_.store(true);
    }
    bool invokeCuller(void *camera, unsigned char mode) noexcept {
        if (!setCuller_)
            return false;
        __try {
            setCuller_(camera, mode);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Opt-in bounded observation. Native reads remain on the owning callbacks;
    // only scalar copies reach the sample mailbox.
    void capture(void *renderer, void *view, bool directCamera) {
        if (!ready_ || !diagnosticsReady_ || !policy_.diagnosticsEnabled() || policy_.stopped())
            return;
        static thread_local std::chrono::steady_clock::time_point next{};
        const auto now = std::chrono::steady_clock::now();
        if (now < next)
            return;
        next = now + std::chrono::milliseconds(500);
        CameraVisibilitySample s;
        s.epoch = policy_.epoch();
        s.milliseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        s.flying = policy_.active();
        if (diagnosticGameEpoch_.load() == s.epoch &&
            s.milliseconds - diagnosticGameTime_.load() <= 1500)
            s.gameType = diagnosticGameType_.load();
        void *camera = directCamera ? renderer : nullptr;
        void *culler{};
        std::uintptr_t vt{};
        if (!directCamera && !world_native::read(renderer, rendererPlayerOffset_, camera))
            return;
        s.valid = camera && world_native::read(camera, cameraPositionOffset_, s.cameraPosition) &&
                  finite(s.cameraPosition) &&
                  world_native::read(camera, renderOriginOffset_, s.origin) && finite(s.origin) &&
                  world_native::read(camera, rawFlagsOffset_, s.rawFlags) &&
                  world_native::read(camera, cullerTypeOffset_, s.cullerType) &&
                  s.cullerType < kCullerModeCount &&
                  world_native::read(camera, cullerOffset_, culler) &&
                  world_native::read(culler, 0, vt);
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (vt >= base && vt - base < 0x20000000)
            s.cullerVtable = vt - base;
        s.viewValid = view && world_native::read(view, viewFlagsOffset_, s.viewFlags) &&
                      std::all_of(s.viewFlags.begin(), s.viewFlags.begin() + viewFlagsCount_,
                                  [](unsigned char b) { return b <= 1; });
        if (s.viewValid)
            s.drawInsideCubes = s.viewFlags[insideCubesIndex_] != 0;
        std::unique_lock lock(sampleMutex_, std::try_to_lock);
        if (lock) {
            sample_ = s;
            sampleReady_ = true;
        }
    }

    // A value-returning native factory owns this output until it returns to its
    // caller. Change only its newly constructed bool, never an already shared view.
    static void *__fastcall createViewDetour(void *camera, void *output, void *screen,
                                             unsigned char subClient) {
        NativeCameraVisual *self = state().createViewOwner;
        const auto world = self->policy_.epoch();
        void *result = self->originalCreateView_(camera, output, screen, subClient);
        if (camera != self->primaryCamera_.load() || self->primaryCameraEpoch_.load() != world ||
            self->policy_.epoch() != world)
            return result;
        self->viewCreated_.fetch_add(1, std::memory_order_relaxed);
        if (!self->policy_.active() || self->policy_.stopped() || !self->policy_.spectatorMode())
            return result;
        std::array<unsigned char, 13> flags{};
        if (result != output || !world_native::read(output, self->viewFlagsOffset_, flags) ||
            !std::all_of(flags.begin(), flags.begin() + self->viewFlagsCount_,
                         [](unsigned char b) { return b <= 1; })) {
            self->viewWriteFailed_.fetch_add(1, std::memory_order_relaxed);
            return result;
        }
        if (flags[self->insideCubesIndex_]) {
            const unsigned char disabled = 0;
            if (world_native::write(output, self->insideCubesOffset_, disabled))
                self->viewChanged_.fetch_add(1, std::memory_order_relaxed);
            else
                self->viewWriteFailed_.fetch_add(1, std::memory_order_relaxed);
        }
        return result;
    }
    // Read-only observation after the factory, at the native camera consumer.
    static void __fastcall cameraRenderDetour(void *camera, void *context, void *view,
                                              void *instance) {
        NativeCameraVisual *self = state().observeOwner;
        if (camera == self->primaryCamera_.load() &&
            self->primaryCameraEpoch_.load() == self->policy_.epoch()) {
            self->cameraRenderCalls_.fetch_add(1, std::memory_order_relaxed);
            self->capture(camera, view, true);
        }
        self->originalCameraRender_(camera, context, view, instance);
    }
    // Intercept the native selector BEFORE its update. Repeated native requests
    // then see an already selected mode5 and do not rebuild the async culler each frame.
    static void __fastcall cullerDetour(void *camera, unsigned char requested) {
        NativeCameraVisual *self = state().cullerOwner;
        const auto world = self->policy_.epoch();
        const auto context = self->nativeModeContext_.load();
        bool rewrite = self->terrainReady_.load(std::memory_order_relaxed) &&
                       requested == kNativeDefaultCullerMode &&
                       camera == self->primaryCamera_.load() &&
                       self->primaryCameraEpoch_.load() == world && (context >> 8) == world &&
                       (context & 0xff) != 7 && self->policy_.active() &&
                       !self->policy_.stopped() && self->policy_.spectatorMode();
        if (!rewrite) {
            self->originalSetCuller_(camera, requested);
            return;
        }
        self->selectorRequests_.fetch_add(1, std::memory_order_relaxed);
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        if (caller >= base && caller - base < 0x20000000)
            self->selectorCaller_.store(caller - base);
        std::unique_lock guard(self->terrainLeaseMutex_, std::try_to_lock);
        if (!guard) {
            self->originalSetCuller_(camera, requested);
            return;
        }
        self->resetCullerBudgetLocked();
        if (self->terrainRejected_.load()) {
            self->originalSetCuller_(camera, requested);
            return;
        }
        unsigned char before{};
        if (!world_native::read(camera, self->cullerTypeOffset_, before) ||
            before >= kCullerModeCount) {
            self->originalSetCuller_(camera, requested);
            return;
        }
        if (self->cullerLease_.owned &&
            (self->cullerLease_.camera != camera || self->cullerLease_.epoch != world ||
             self->cullerLease_.nativeModeContext != context))
            self->cullerLease_ = {};
        if (!self->cullerLease_.owned) {
            self->cullerLease_ = {camera, world, requested, true, context};
            self->terrainLeaseActive_.store(true);
        }
        self->selectorRewrites_.fetch_add(1, std::memory_order_relaxed);
        self->originalSetCuller_(camera, self->spectatorMode_);
        unsigned char after{};
        if (world_native::read(camera, self->cullerTypeOffset_, after)) {
            self->terrainObservedMode_.store(after);
            self->terrainPending_.store(after != self->spectatorMode_);
            if (before != self->spectatorMode_ && after == self->spectatorMode_)
                self->recordCullerSwitchLocked();
        }
    }

    struct CullerLease {
        void *camera{};
        std::uint64_t epoch{};
        unsigned char previous{};
        bool owned{};
        std::uint64_t nativeModeContext{};
    };

    Policy policy_{};
    nlohmann::json root_;
    bool ready_{}, diagnosticsReady_{};
    std::uint32_t rendererPlayerOffset_{}, renderOriginOffset_{};
    std::string behavior_;
    std::uint32_t terrainSchema_{};
    unsigned char spectatorMode_{};
    nlohmann::json setCullerDef_, factoryCheckDef_, spectatorUpdateDef_;
    std::uint32_t cullerOffset_{}, cullerTypeOffset_{};
    std::uint32_t spectatorVtableRva_{};
    std::uintptr_t spectatorVtable_{};
    nlohmann::json observeDef_, createViewDef_;
    std::uint32_t viewFlagsOffset_{}, viewFlagsCount_{}, insideCubesOffset_{}, insideCubesIndex_{};
    std::uint32_t cameraPositionOffset_{}, rawFlagsOffset_{};

    SetCullerFn setCuller_{}, originalSetCuller_{};
    CameraRenderFn originalCameraRender_{};
    CreateViewFn originalCreateView_{};
    BDS_HookToken cullerToken_{}, cameraRenderToken_{}, createViewToken_{};
    std::atomic<bool> terrainReady_{}, observeReady_{}, createViewReady_{};
    std::atomic<unsigned> cameraRenderCalls_{}, viewCreated_{}, viewChanged_{}, viewWriteFailed_{};
    std::atomic<unsigned> terrainCalls_{}, terrainChanges_{}, terrainRestores_{}, terrainFaults_{};
    std::atomic<int> terrainObservedMode_{-1};
    std::atomic<bool> terrainPending_{}, terrainLeaseActive_{}, terrainRejected_{};
    std::atomic<unsigned> selectorRequests_{}, selectorRewrites_{};
    std::atomic<std::uintptr_t> selectorCaller_{};
    std::atomic<void *> primaryCamera_{};
    std::atomic<std::uint64_t> primaryCameraEpoch_{};
    std::atomic<std::uint64_t> nativeModeContext_{};
    std::atomic<int> diagnosticGameType_{-1};
    std::atomic<std::uint64_t> diagnosticGameEpoch_{}, diagnosticGameTime_{};
    // All culler ownership bookkeeping belongs to the native render callback.
    // Workers only read atomic diagnostics; they never access camera/culler pointers.
    CullerLease cullerLease_;
    std::mutex terrainLeaseMutex_;
    std::uint64_t budgetFlight_{};
    unsigned budgetSwitches_{};
    std::mutex sampleMutex_;
    CameraVisibilitySample sample_{};
    bool sampleReady_{};
};
} // namespace bedrock
