#include "bedrock/sdk/camera_control.hpp"

#include "profile_facade.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#ifdef _WIN32
#include <bedrock/detail/camera_rig.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/generated/symbols.hpp>
#include <bedrock/native/actor_components.hpp>
#include <bedrock/native/actors.hpp>
#include <bedrock/native/camera.hpp>
#include <bedrock/native/camera_visual.hpp>
#include <bedrock/native/game_type.hpp>
#include <bedrock/native/inside_block_effect.hpp>
#include <bedrock/native/motion.hpp>
#include <bedrock/native/outline_renderer.hpp>
#include <bedrock/native/packet_sender.hpp>
#include <bedrock/native/player_bounds_tracker.hpp>
#include <bedrock/native/world_render.hpp>
#include <bedrock/packets/wire_packet.hpp>
#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/runtime/hook_runtime.h>

#include <nlohmann/json.hpp>
#include <windows.h>

#include <array>
#include <chrono>
#include <cstring>
#include <vector>
#endif

namespace bedrock::sdk {

namespace {

#ifdef _WIN32
CameraChange fromNative(bedrock::CameraChange change) noexcept {
    switch (change) {
    case bedrock::CameraChange::Applied: return CameraChange::Applied;
    case bedrock::CameraChange::NoPlayer: return CameraChange::NoPlayer;
    case bedrock::CameraChange::WrongThread: return CameraChange::WrongThread;
    case bedrock::CameraChange::WorldChanged: return CameraChange::WorldChanged;
    case bedrock::CameraChange::InvalidValue: return CameraChange::InvalidValue;
    case bedrock::CameraChange::NativeFailure: return CameraChange::NativeFailure;
    }
    return CameraChange::NativeFailure;
}

bedrock::Vec3 toNative(Vec3Value value) noexcept { return {value.x, value.y, value.z}; }
Vec3Value fromNative(bedrock::Vec3 value) noexcept { return {value.x, value.y, value.z}; }
bedrock::ActorBounds toNative(ActorBoundsValue value) noexcept {
    return {{value.minimum.x, value.minimum.y, value.minimum.z},
            {value.maximum.x, value.maximum.y, value.maximum.z}};
}
ActorBoundsValue fromNative(const bedrock::ActorBounds& value) noexcept {
    return {{value.minimum.x, value.minimum.y, value.minimum.z},
            {value.maximum.x, value.maximum.y, value.maximum.z}, value.valid()};
}
// Default inside-block-effect policy: never suppress. The effect filter still
// observes and counts, it only leaves the native result untouched.
bool noEffectSuppression() noexcept { return false; }
#endif

} // namespace

struct CameraControl::Impl {
#ifdef _WIN32
    bedrock::NativeCamera nativeCamera;
    bedrock::NativeCameraVisual cameraVisual;
    bedrock::NativeInsideBlockEffect insideBlockEffect;
    bedrock::NativeOutlineRenderer outlineRenderer;
    bedrock::NativeGameType gameTypes;
    bedrock::NativeActorLayout actors;
    bedrock::NativeActorComponents actorComponents;
    bedrock::NativePlayerBoundsTracker playerBounds;
    bedrock::WorldRenderProfile render;
    bedrock::native_motion::Layout motionLayout;
    bedrock::CameraRig cameraRig;
    bedrock::NativePacketSender senderLayout;
    std::optional<bedrock::NativeCamera::Scope> cameraScope;
    std::optional<bedrock::NativeGameType::Scope> gameTypeScope;

    bool renderReady{};
    unsigned visualRevision{};
    std::uint32_t hurtHash{}, fallHash{};
    std::size_t hurtOffset{}, fallOffset{};
    CameraEffectPolicy effectPolicy{&noEffectSuppression};
    CameraRenderHandler renderHandler{};
    void* renderUser{};
    BDS_HookToken renderToken{};

    static void BDS_HOOK_CALL renderHook(void* user, void* raw, BDS_HookNext next,
                                        void* frame) noexcept {
        auto& self = *static_cast<Impl*>(user);
        auto& args = *static_cast<BDS_RenderLevelArgs*>(raw);
        try {
            if (auto* camera = self.outlineRenderer.renderPlayer(args.renderer))
                self.cameraVisual.notePrimaryCamera(camera);
            self.cameraVisual.updateTerrain(args.renderer);
            if (!self.cameraVisual.observeReady())
                self.cameraVisual.captureFromRenderer(args.renderer);
        } catch (...) {
        }
        next(frame, raw);
        if (!self.renderHandler) return;
        CameraRenderFrame borrowed{args.renderer, args.screen};
        self.renderHandler(borrowed, self.renderUser);
    }
#endif
};

CameraControl::CameraControl() : impl_(std::make_unique<Impl>()) {}
CameraControl::~CameraControl() { stopRenderHook(); }
CameraControl::CameraControl(CameraControl&&) noexcept = default;
CameraControl& CameraControl::operator=(CameraControl&&) noexcept = default;

bool CameraControl::configure(const Profile& profile, const CameraVisualPolicy& policy,
                              std::string& error, std::vector<std::string>& warnings) {
#ifdef _WIN32
    warnings.clear();
    const auto runtime = ProfileRuntimeAccess::runtime(profile);
    if (!runtime) {
        error = "camera profile runtime snapshot unavailable";
        return false;
    }
    try {
        if (!runtime->supports({bedrock::Feature::Camera, bedrock::Feature::Motion}, error))
            return false;
        const auto root = bedrock::parseNativeProfile(runtime->json());
        const auto& camera = root.at("native_camera");
        if (camera.at("schema") != 1 || camera.at("pointer_size") != 8) {
            error = "unsupported Freecam camera layout";
            return false;
        }
        impl_->hurtHash = bedrock::native_profile::number(camera.at("hurt_time_component_hash"));
        impl_->hurtOffset = bedrock::native_profile::number(camera.at("hurt_time_value_offset"));
        if (impl_->hurtOffset > 4096 || camera.at("hurt_time_value_size") != 4) {
            error = "invalid Freecam camera/hurt layout";
            return false;
        }
        const auto& visual = camera.at("visual");
        if (visual.value("revision", 0) < camera.value("minimum_visual_revision", 0)) {
            error = "Freecam visual definition predates the InsideBlockEffect port; the exact "
                    "embedded profile is required";
            return false;
        }
        if (visual.at("schema") != 1) {
            error = "unsupported camera visual schema";
            return false;
        }
        impl_->visualRevision = visual.value("revision", 9u);

        impl_->motionLayout = bedrock::native_motion::Layout::fromProfile(runtime->json());
        const auto statePosition =
            bedrock::native_profile::number(root.at("native_motion").at("state_vector").at(
                "state_position_offset"));
        impl_->fallHash = impl_->motionLayout.fallHash;
        impl_->fallOffset = impl_->motionLayout.fall;
        if (statePosition != 0 || impl_->motionLayout.previous != 12 ||
            impl_->motionLayout.velocity != 24 || impl_->fallOffset != 0) {
            error = "unsupported local Player motion layout";
            return false;
        }

        std::string actorError;
        if (!bedrock::parseNativeActorLayout(runtime->json(), impl_->actors, actorError)) {
            error = actorError;
            return false;
        }
        impl_->actorComponents.configure(impl_->actors, root);
        impl_->senderLayout.load(root);

        std::string renderError;
        impl_->renderReady =
            bedrock::parseWorldRenderProfile(runtime->json(), impl_->render, renderError);
        if (impl_->renderReady) impl_->playerBounds.configure(impl_->actors, impl_->render);
        else warnings.push_back("player hitbox render layout unavailable: " + renderError);

        if (!impl_->nativeCamera.load(*runtime, error)) return false;

        std::string gameTypeError;
        if (!impl_->gameTypes.load(*runtime, gameTypeError))
            warnings.push_back("native game type observation unavailable: " + gameTypeError);

        if (impl_->renderReady) {
            std::string outlineError;
            if (!impl_->outlineRenderer.load(*runtime, outlineError)) {
                impl_->renderReady = false;
                warnings.push_back("native drawing unavailable: " + outlineError);
            }
        }

        const bedrock::NativeCameraVisual::Policy visualPolicy{
            policy.active, policy.stopped, policy.spectatorMode,
            policy.diagnosticsEnabled, policy.epoch, policy.flightSerial};
        std::string visualError;
        if (!impl_->cameraVisual.load(root, impl_->render.player, impl_->render.origin, visualPolicy,
                                      visualError)) {
            warnings.push_back("camera visual unavailable: " + visualError);
        } else {
            std::string terrainError;
            if (impl_->cameraVisual.installTerrain(impl_->renderReady, terrainError))
                warnings.push_back("spectator culler ready [native selector hook; "
                                   "profile-defined vtable; game mode unchanged]");
            else
                warnings.push_back("terrain culling unavailable: " + terrainError);
            std::string observeError;
            if (impl_->cameraVisual.installRenderObserve(impl_->renderReady, observeError))
                warnings.push_back("native ViewRenderData observer ready [read only; no overlay "
                                   "suppression]");
            else
                warnings.push_back("ViewRenderData observer unavailable: " + observeError);
            std::string createViewError;
            if (impl_->cameraVisual.installCreateView(createViewError))
                warnings.push_back("view factory ready [new output draw_inside_cubes=false; no "
                                   "game-mode/ability writes]");
            else
                warnings.push_back("view factory unavailable: " + createViewError);
        }

        std::string effectError;
        if (impl_->insideBlockEffect.install(*runtime, bedrock::generated::kSymbolDatabase,
                                             impl_->effectPolicy, effectError))
            warnings.push_back("RenderDragon InsideBlockEffect filter ready [SDK tag + paired "
                               "extraction caller; versioned visual profile]");
        else
            warnings.push_back("RenderDragon InsideBlockEffect filter unavailable: " + effectError);

        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
#else
    (void)profile;
    (void)policy;
    (void)warnings;
    error = "native camera control unavailable on this host";
    return false;
#endif
}

unsigned CameraControl::visualRevision() const noexcept {
#ifdef _WIN32
    return impl_->visualRevision;
#else
    return 0;
#endif
}

void CameraControl::reset() noexcept {
#ifdef _WIN32
    impl_->cameraScope.reset();
    impl_->gameTypeScope.reset();
    impl_->nativeCamera.reset();
    impl_->gameTypes.reset();
#endif
}

bool CameraControl::tick(void* clientInstance, std::uint64_t worldEpoch) noexcept {
#ifdef _WIN32
    impl_->cameraScope.reset();
    impl_->gameTypeScope.reset();
    auto scope = impl_->nativeCamera.tick(clientInstance, bedrock::WorldEpoch{worldEpoch});
    const bool ok = static_cast<bool>(scope);
    if (ok) impl_->cameraScope.emplace(std::move(scope));
    // The game-type context has its own tick scope; freecam reads gameType()
    // inside the same LevelTick window, so tick it alongside the camera.
    impl_->gameTypeScope.emplace(
        impl_->gameTypes.tick(clientInstance, bedrock::WorldEpoch{worldEpoch}));
    return ok;
#else
    (void)clientInstance;
    (void)worldEpoch;
    return false;
#endif
}

void* CameraControl::localActor() const noexcept {
#ifdef _WIN32
    return impl_->nativeCamera.localActor();
#else
    return nullptr;
#endif
}

std::optional<CameraPlayerSnapshot> CameraControl::readPlayerProxy() const noexcept {
#ifdef _WIN32
    const auto snapshot = impl_->nativeCamera.readPlayerProxy();
    if (!snapshot) return std::nullopt;
    CameraPlayerSnapshot result;
    result.runtimeId = snapshot->runtimeId;
    result.worldEpoch = snapshot->worldEpoch;
    result.position = fromNative(snapshot->position);
    result.previous = fromNative(snapshot->previous);
    result.velocity = fromNative(snapshot->velocity);
    result.fallDistance = snapshot->fallDistance;
    result.bounds = fromNative(snapshot->bounds);
    result.rotation_pitch = snapshot->rotation.pitch;
    result.rotation_yaw = snapshot->rotation.yaw;
    return result;
#else
    return std::nullopt;
#endif
}

CameraChange CameraControl::applyPlayerProxy(const CameraFrame& frame, std::uint64_t runtimeId,
                                             std::uint64_t epoch) noexcept {
#ifdef _WIN32
    bedrock::CameraFrame nativeFrame;
    nativeFrame.position = toNative(frame.position);
    nativeFrame.previous = toNative(frame.previous);
    nativeFrame.bounds = toNative(frame.bounds);
    return fromNative(impl_->nativeCamera.applyPlayerProxy(nativeFrame, runtimeId,
                                                           bedrock::WorldEpoch{epoch}));
#else
    (void)frame;
    (void)runtimeId;
    (void)epoch;
    return CameraChange::NoPlayer;
#endif
}

CameraChange CameraControl::restorePlayerProxy(const CameraPlayerSnapshot& saved) noexcept {
#ifdef _WIN32
    bedrock::CameraPlayerSnapshot nativeSaved;
    nativeSaved.runtimeId = saved.runtimeId;
    nativeSaved.worldEpoch = saved.worldEpoch;
    nativeSaved.position = toNative(saved.position);
    nativeSaved.previous = toNative(saved.previous);
    nativeSaved.velocity = toNative(saved.velocity);
    nativeSaved.fallDistance = saved.fallDistance;
    nativeSaved.bounds = toNative(saved.bounds);
    nativeSaved.rotation = {saved.rotation_pitch, saved.rotation_yaw};
    return fromNative(impl_->nativeCamera.restorePlayerProxy(nativeSaved));
#else
    (void)saved;
    return CameraChange::NoPlayer;
#endif
}

bool CameraControl::beginCamera(Vec3Value position, ActorBoundsValue bounds,
                                std::uint64_t runtimeId, std::uint64_t epoch) noexcept {
#ifdef _WIN32
    return impl_->cameraRig.begin(toNative(position), toNative(bounds), runtimeId, epoch);
#else
    (void)position;
    (void)bounds;
    (void)runtimeId;
    (void)epoch;
    return false;
#endif
}

std::optional<CameraFrame> CameraControl::advanceCamera(float yaw, const CameraMoveInput& input,
                                                        const CameraMoveSpeed& speed,
                                                        float elapsedSeconds,
                                                        std::uint64_t runtimeId,
                                                        std::uint64_t epoch) noexcept {
#ifdef _WIN32
    // bedrock::Yaw rejects non-finite input by throwing, and this entry point
    // is noexcept: validate here instead of letting it terminate the tick.
    if (!std::isfinite(yaw)) return std::nullopt;
    const auto next = impl_->cameraRig.advance(
        bedrock::Yaw{yaw}, {input.forward, input.right, input.up},
        {speed.horizontal, speed.vertical}, std::chrono::duration<float>{elapsedSeconds},
        runtimeId, epoch);
    if (!next) return std::nullopt;
    CameraFrame result;
    result.position = fromNative(next->position);
    result.previous = fromNative(next->previous);
    result.bounds = fromNative(next->bounds);
    return result;
#else
    (void)yaw;
    (void)input;
    (void)speed;
    (void)elapsedSeconds;
    (void)runtimeId;
    (void)epoch;
    return std::nullopt;
#endif
}

void CameraControl::resetCamera() noexcept {
#ifdef _WIN32
    impl_->cameraRig.reset();
#endif
}

bool CameraControl::cameraActive() const noexcept {
#ifdef _WIN32
    return impl_->cameraRig.active();
#else
    return false;
#endif
}

CameraFrame CameraControl::frame() const noexcept {
#ifdef _WIN32
    const auto current = impl_->cameraRig.frame();
    if (!current) return {};
    CameraFrame result;
    result.position = fromNative(current->position);
    result.previous = fromNative(current->previous);
    result.bounds = fromNative(current->bounds);
    return result;
#else
    return {};
#endif
}

CameraGameType CameraControl::gameType() const noexcept {
#ifdef _WIN32
    const auto value = impl_->gameTypes.gameType();
    if (!value) return {0, false};
    return {static_cast<int>(*value), true};
#else
    return {0, false};
#endif
}

bool CameraControl::installTerrain(bool rendererReady, std::string& error) {
#ifdef _WIN32
    return impl_->cameraVisual.installTerrain(rendererReady, error);
#else
    (void)rendererReady;
    error = "unavailable";
    return false;
#endif
}

bool CameraControl::installRenderObserve(bool rendererReady, std::string& error) {
#ifdef _WIN32
    return impl_->cameraVisual.installRenderObserve(rendererReady, error);
#else
    (void)rendererReady;
    error = "unavailable";
    return false;
#endif
}

bool CameraControl::installCreateView(std::string& error) {
#ifdef _WIN32
    return impl_->cameraVisual.installCreateView(error);
#else
    error = "unavailable";
    return false;
#endif
}

bool CameraControl::renderReady() const noexcept {
#ifdef _WIN32
    return impl_->renderReady;
#else
    return false;
#endif
}

bool CameraControl::terrainReady() const noexcept {
#ifdef _WIN32
    return impl_->cameraVisual.terrainReady();
#else
    return false;
#endif
}

bool CameraControl::observeReady() const noexcept {
#ifdef _WIN32
    return impl_->cameraVisual.observeReady();
#else
    return false;
#endif
}

bool CameraControl::createViewReady() const noexcept {
#ifdef _WIN32
    return impl_->cameraVisual.createViewReady();
#else
    return false;
#endif
}

bool CameraControl::leaseActive() const noexcept {
#ifdef _WIN32
    return impl_->cameraVisual.leaseActive();
#else
    return false;
#endif
}

void CameraControl::updateTerrain(void* renderer) {
#ifdef _WIN32
    impl_->cameraVisual.updateTerrain(renderer);
#else
    (void)renderer;
#endif
}

void CameraControl::captureFromRenderer(void* renderer) {
#ifdef _WIN32
    impl_->cameraVisual.captureFromRenderer(renderer);
#else
    (void)renderer;
#endif
}

std::uint64_t CameraControl::primaryCameraEpoch() const noexcept {
#ifdef _WIN32
    return impl_->cameraVisual.primaryCameraEpoch();
#else
    return 0;
#endif
}

void CameraControl::notePrimaryCamera(void* camera) noexcept {
#ifdef _WIN32
    impl_->cameraVisual.notePrimaryCamera(camera);
#else
    (void)camera;
#endif
}

void CameraControl::noteNativeModeContext(std::uint64_t context) noexcept {
#ifdef _WIN32
    impl_->cameraVisual.noteNativeModeContext(context);
#else
    (void)context;
#endif
}

void CameraControl::setDiagnosticGameType(int type, std::uint64_t epoch,
                                          std::uint64_t milliseconds) noexcept {
#ifdef _WIN32
    impl_->cameraVisual.setDiagnosticGameType(type, epoch, milliseconds);
#else
    (void)type;
    (void)epoch;
    (void)milliseconds;
#endif
}

std::optional<CameraVisibilitySample> CameraControl::takeSample() noexcept {
#ifdef _WIN32
    const auto sample = impl_->cameraVisual.takeSample();
    if (!sample) return std::nullopt;
    CameraVisibilitySample result;
    result.epoch = sample->epoch;
    result.milliseconds = sample->milliseconds;
    result.gameType = sample->gameType;
    result.flying = sample->flying;
    result.valid = sample->valid;
    result.cameraPosition_x = sample->cameraPosition.x;
    result.cameraPosition_y = sample->cameraPosition.y;
    result.cameraPosition_z = sample->cameraPosition.z;
    result.origin_x = sample->origin.x;
    result.origin_y = sample->origin.y;
    result.origin_z = sample->origin.z;
    std::copy(std::begin(sample->rawFlags), std::end(sample->rawFlags),
              std::begin(result.rawFlags));
    result.cullerType = sample->cullerType;
    result.cullerVtable = sample->cullerVtable;
    result.viewValid = sample->viewValid;
    std::copy(std::begin(sample->viewFlags), std::end(sample->viewFlags),
              std::begin(result.viewFlags));
    result.drawInsideCubes = sample->drawInsideCubes;
    return result;
#else
    return std::nullopt;
#endif
}

CameraVisualStatistics CameraControl::takeStatistics() noexcept {
#ifdef _WIN32
    const auto stats = impl_->cameraVisual.takeStatistics();
    CameraVisualStatistics result;
    result.cameraRenderCalls = stats.cameraRenderCalls;
    result.viewCreated = stats.viewCreated;
    result.viewChanged = stats.viewChanged;
    result.viewWriteFailed = stats.viewWriteFailed;
    result.terrainCalls = stats.terrainCalls;
    result.terrainChanges = stats.terrainChanges;
    result.terrainRestores = stats.terrainRestores;
    result.terrainFaults = stats.terrainFaults;
    result.selectorRequests = stats.selectorRequests;
    result.selectorRewrites = stats.selectorRewrites;
    result.selectorCaller = stats.selectorCaller;
    result.terrainObservedMode = stats.terrainObservedMode;
    result.terrainPending = stats.terrainPending;
    result.terrainRejected = stats.terrainRejected;
    return result;
#else
    return {};
#endif
}

CameraVisualStatistics CameraControl::statistics() const noexcept {
#ifdef _WIN32
    const auto stats = impl_->cameraVisual.statistics();
    CameraVisualStatistics result;
    result.cameraRenderCalls = stats.cameraRenderCalls;
    result.viewCreated = stats.viewCreated;
    result.viewChanged = stats.viewChanged;
    result.viewWriteFailed = stats.viewWriteFailed;
    result.terrainCalls = stats.terrainCalls;
    result.terrainChanges = stats.terrainChanges;
    result.terrainRestores = stats.terrainRestores;
    result.terrainFaults = stats.terrainFaults;
    result.selectorRequests = stats.selectorRequests;
    result.selectorRewrites = stats.selectorRewrites;
    result.selectorCaller = stats.selectorCaller;
    result.terrainObservedMode = stats.terrainObservedMode;
    result.terrainPending = stats.terrainPending;
    result.terrainRejected = stats.terrainRejected;
    return result;
#else
    return {};
#endif
}

void CameraControl::setEffectPolicy(CameraEffectPolicy policy) noexcept {
#ifdef _WIN32
    // NativeInsideBlockEffect copies the policy pointer when it installs the
    // filter, so this must be called before configure(). The default policy
    // never suppresses the effect.
    impl_->effectPolicy = policy ? policy : &noEffectSuppression;
#else
    (void)policy;
#endif
}

bool CameraControl::effectReady() const noexcept {
#ifdef _WIN32
    return impl_->insideBlockEffect.ready();
#else
    return false;
#endif
}

CameraEffectStatistics CameraControl::takeEffectStatistics() noexcept {
#ifdef _WIN32
    const auto stats = impl_->insideBlockEffect.takeStatistics();
    CameraEffectStatistics result;
    result.ready = impl_->insideBlockEffect.ready();
    result.calls = stats.calls;
    result.eligible = stats.eligible;
    result.skipped = stats.skipped;
    result.otherCaller = stats.otherCaller;
    result.callerRva = stats.callerRva;
    return result;
#else
    return {};
#endif
}

bool CameraControl::hurtTime(void* actor, int& value) const noexcept {
#ifdef _WIN32
    if (!bedrock::native_profile::read(impl_->actorComponents.component(actor, impl_->hurtHash),
                                       impl_->hurtOffset, value))
        return false;
    return value >= 0 && value < 10000;
#else
    (void)actor;
    (void)value;
    return false;
#endif
}

std::optional<std::uint64_t> CameraControl::actorRuntimeId(void* actor) const noexcept {
#ifdef _WIN32
    std::uint64_t id{};
    if (!bedrock::native_profile::read(
            impl_->actorComponents.component(actor, impl_->actors.runtimeIdComponentHash),
            impl_->actors.runtimeIdValueOffset, id))
        return std::nullopt;
    return id;
#else
    (void)actor;
    return std::nullopt;
#endif
}

bool CameraControl::actorIdentity(void* actor, std::uintptr_t& registry,
                                  std::uint32_t& entity) const noexcept {
#ifdef _WIN32
    void* context{};
    if (!bedrock::native_profile::read(
            actor, impl_->actors.actorEntityContextOffset + impl_->actors.contextRegistryOffset,
            context) ||
        !context ||
        !bedrock::native_profile::read(
            actor, impl_->actors.actorEntityContextOffset + impl_->actors.contextEntityIdOffset,
            entity))
        return false;
    registry = reinterpret_cast<std::uintptr_t>(context);
    return true;
#else
    (void)actor;
    (void)registry;
    (void)entity;
    return false;
#endif
}

bool CameraControl::restoreActorMotion(void* actor, Vec3Value position,
                                       ActorBoundsValue bounds) noexcept {
#ifdef _WIN32
    const auto get = [this](void* value, std::uint32_t hash) {
        return impl_->actorComponents.component(value, hash);
    };
    // Position restore is a teleport, not a frame-long interpolation across the
    // flight path: motion and fall distance are cleared when returning.
    const bedrock::native_motion::Snapshot restored{toNative(position), toNative(position),
                                                    bedrock::Vec3{}, toNative(bounds), 0.f};
    return bedrock::native_motion::apply(actor, impl_->actors, impl_->motionLayout, get, restored);
#else
    (void)actor;
    (void)position;
    (void)bounds;
    return false;
#endif
}

bool CameraControl::capturePlayerBounds(void* localActor, std::uint64_t epoch, unsigned rows,
                                        PlayerBoundsFrame& out) noexcept {
#ifdef _WIN32
    if (!localActor) return false;
    bedrock::ActorBoundsFrame batch;
    batch.epoch = epoch;
    batch.origin = toNative(out.origin);
    if (!impl_->playerBounds.capture(localActor, &batch, rows)) return false;
    out.epoch = batch.epoch;
    out.madeNanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(batch.made.time_since_epoch()).count();
    out.count = std::min<unsigned>(batch.count, static_cast<unsigned>(out.boxes.size()));
    for (unsigned i = 0; i < out.count; ++i) {
        out.boxes[i].bounds = fromNative(batch.boxes[i].bounds);
        out.boxes[i].blendFrom = fromNative(batch.boxes[i].blendFrom);
    }
    out.origin = fromNative(batch.origin);
    return true;
#else
    (void)localActor;
    (void)epoch;
    (void)rows;
    (void)out;
    return false;
#endif
}

bool CameraControl::resolveSender(void* client, void*& sender, void*& target) const noexcept {
#ifdef _WIN32
    return impl_->senderLayout.resolve(client, sender, target);
#else
    (void)client;
    (void)sender;
    (void)target;
    return false;
#endif
}

bool CameraControl::installRenderHook(CameraRenderHandler handler, void* user,
                                      std::string& error) {
#ifdef _WIN32
    if (!handler) {
        error = "render handler is missing";
        return false;
    }
    auto* target = impl_->outlineRenderer.renderTarget();
    if (!target) {
        error = "native render target unavailable";
        return false;
    }
    impl_->renderHandler = handler;
    impl_->renderUser = user;
    if (impl_->renderToken) {
        error.clear();
        return true;
    }
    if (!BDS_HookSubscribe(BDS_HOOK_RENDER_LEVEL, target, &Impl::renderHook, impl_.get(),
                           &impl_->renderToken)) {
        impl_->renderHandler = nullptr;
        impl_->renderUser = nullptr;
        error = "render hook conflict/install failure; restart Minecraft with matching runtime DLLs";
        return false;
    }
    error.clear();
    return true;
#else
    (void)handler;
    (void)user;
    error = "native render hook unavailable on this host";
    return false;
#endif
}

void CameraControl::stopRenderHook() noexcept {
#ifdef _WIN32
    if (impl_->renderToken) BDS_HookUnsubscribe(impl_->renderToken);
    impl_->renderToken = nullptr;
    impl_->renderHandler = nullptr;
    impl_->renderUser = nullptr;
#endif
}

void* CameraControl::renderTarget() const noexcept {
#ifdef _WIN32
    return impl_->outlineRenderer.renderTarget();
#else
    return nullptr;
#endif
}

CameraDrawResult CameraControl::drawLines(void* renderer, void* screen, Vec3Value origin,
                                          const CameraLineSegment* segments, std::size_t count,
                                          std::array<float, 4> rgba) noexcept {
#ifdef _WIN32
    if (!renderer || !screen || !segments || !count) return CameraDrawResult::Skipped;
    try {
        bedrock::WorldOutlineGeometry geometry;
        geometry.origin = toNative(origin);
        bedrock::WorldGeometryBatch batch;
        batch.color = rgba;
        batch.vertices.reserve(count * 2);
        for (std::size_t i = 0; i < count; ++i) {
            batch.vertices.push_back(toNative(segments[i].from));
            batch.vertices.push_back(toNative(segments[i].to));
        }
        geometry.lines.push_back(std::move(batch));
        switch (impl_->outlineRenderer.draw(renderer, screen, geometry)) {
        case bedrock::OutlineDraw::Drawn: return CameraDrawResult::Drawn;
        case bedrock::OutlineDraw::Fault: return CameraDrawResult::Fault;
        case bedrock::OutlineDraw::Skipped: return CameraDrawResult::Skipped;
        }
        return CameraDrawResult::Skipped;
    } catch (...) {
        return CameraDrawResult::Fault;
    }
#else
    (void)renderer;
    (void)screen;
    (void)origin;
    (void)segments;
    (void)count;
    (void)rgba;
    return CameraDrawResult::Skipped;
#endif
}

} // namespace bedrock::sdk
