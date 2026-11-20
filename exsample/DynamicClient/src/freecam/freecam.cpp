#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/sdk/keybinds.hpp>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <bedrock/detail/hook_dispatch.hpp>
#include <bedrock/native/motion.hpp>
#include <bedrock/native/packet_sender.hpp>
#include <bedrock/packets/wire_packet_events.hpp>
#include <bedrock/packets/movement_packets.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include "client.hpp"
#include "worker.hpp"
#include <bedrock/native/outline_renderer.hpp>
#include <bedrock/native/inside_block_effect.hpp>
#include <bedrock/native/camera.hpp>
#include <bedrock/native/camera_visual.hpp>
#include <bedrock/native/game_type.hpp>
#include <bedrock/runtime/game_hooks.hpp>
#include <bedrock/generated/symbols.hpp>
#include <bedrock/native/actors.hpp>
#include <bedrock/native/actor_components.hpp>
#include <bedrock/native/player_bounds_tracker.hpp>
#include <bedrock/native/items.hpp>
#include <bedrock/native/world_render.hpp>
#include <bedrock/profiles/remote_catalog.hpp>
#include <bedrock/profiles/profile_manager.hpp>
#include "freecam/config.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#if !defined(_MSC_VER) || _ITERATOR_DEBUG_LEVEL != 0
#error "Freecam requires MSVC Release STL layout."
#endif
// 許可されていないサーバーでは BAN される可能性があり、保証はしない。自己責任で使うこと。
namespace dynamic_client {
namespace freecam_detail {
std::atomic<bool> stop{}, desired{}, active{}, gameplay{}, restoreRequested{};
unsigned visualRevision = 9;
std::string profileGameVersion, profileExeSha;
std::atomic<void *> client{}, local{};
std::atomic<std::uint64_t> epoch{1};
std::array<std::atomic<bool>, 256> keys{};
std::atomic<std::shared_ptr<const freecam::Config>> config{
    std::make_shared<const freecam::Config>()};
bedrock::detail::HookDispatch api;
std::unique_ptr<bedrock::GameHooks> hooks;
bedrock::NativeActorLayout actors;
bedrock::NativeActorComponents actorComponents;
bedrock::NativePlayerBoundsTracker playerBounds;
bedrock::WorldRenderProfile render;
bedrock::NativeOutlineRenderer outline;
std::size_t hurtOffset{};
std::uint32_t hurtHash{};
BDS_HookToken renderToken{};
bedrock::NativeCamera nativeCamera;
bedrock::NativeCameraVisual cameraVisual;
bedrock::NativeGameType gameTypes;
bedrock::NativeInsideBlockEffect insideBlockEffect;
std::atomic<std::uint64_t> flightSerial{};


std::mutex mailboxMutex, logMutex;
Capture mailbox{};
bool mailboxReady{};
std::atomic<std::shared_ptr<const Mesh>> mesh;
std::ofstream logFile;
std::atomic<unsigned> logBits{};
void note(unsigned flag) {
    logBits.fetch_or(flag, std::memory_order_relaxed);
}
void log(std::string_view text) {
    std::lock_guard guard(logMutex);
    if (logFile) {
        logFile << text << '\n';
        logFile.flush();
    }
}
bool finite(Vec v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
           std::abs(v.x) < 40000000 && std::abs(v.y) < 40000000 && std::abs(v.z) < 40000000;
}
using bedrock::native_profile::read;
bool insideBlockEffectPolicy() noexcept {
    return active.load() && !stop.load() && gameplay.load() &&
           cameraVisual.primaryCameraEpoch() == epoch.load() && config.load()->spectatorMode;
}
void disable(unsigned reason = Disabled) {
    restoreRequested.store(true);
    desired.store(false);
    active.store(false);
    mesh.store(nullptr);
    note(reason);
}
void reset() {
    disable(World);
    nativeCamera.reset();
    gameTypes.reset();
    epoch.fetch_add(1);
    local.store(nullptr);
    for (auto &key : keys)
        key.store(false);
}
void *component(void *actor,std::uint32_t hash) noexcept {return actorComponents.component(actor,hash);}
bool hurtTime(void *actor, int &value) noexcept {
    return read(component(actor, hurtHash), hurtOffset, value) && value >= 0 && value < 10000;
}
void observePacketSender(void *) noexcept;
bool isMovement(unsigned key, const freecam::Config &c) {
    return key == c.forward || key == c.back || key == c.left || key == c.right || key == c.up ||
           key == c.down || key == c.fast;
}
thread_local bool releasing = false;
void releaseMovement(const freecam::Config &c) {
    releasing = true;
    for (unsigned key : {c.forward, c.back, c.left, c.right, c.up, c.down, c.fast}) {
        BDS_KeyboardArgs args{static_cast<int>(key), 0, 0};
        BDS_HookCallOriginal(BDS_HOOK_KEYBOARD_FEED, &args);
    }
    (void)api.sendMouseInput(1, false);
    (void)api.sendMouseInput(2, false);
    releasing = false;
}
void keyEvent(const bedrock::KeyEvent &e) {
    if (releasing || stop.load())
        return;
    const auto c = config.load();
    const unsigned k = e.inputKey();
    if (k < keys.size())
        keys[k].store(e.pressed);
    if (k == boundKey("freecam", bedrock::sdk::Key::M)) {
        // Toggle is handled through the SDK keybind so Mod KeyBind owns the binding.
        if (gameplay.load() && c->enabled)
            e.cancel();
        return;
    }
    if (desired.load() && isMovement(k, *c))
        e.cancel();
}
void mouseEvent(const bedrock::MouseEvent &e) {
    if (releasing || stop.load() || !desired.load())
        return;
    const auto c = config.load();
    if (e.action == 0)
        return;
    else if (!c->allowInteraction &&
             (e.action == 1 || e.action == 2 || e.action == 3 || e.action == 4))
        e.cancel();
}
// Borion-style local Player proxy. All mutable native access is confined to
// LevelTick/world teardown callbacks; the network middleware reads atomics only.
struct BorionState {
    bool saved{};
    void *actor{};
    std::uint64_t epoch{}, runtime{};
    void *registry{};
    std::uint32_t entity{};
    Vec oldPosition{}, oldPrevious{}, oldVelocity{}, position{};
    bedrock::ActorBounds oldBounds{};
    float oldFall{};
};
BorionState borion;
bedrock::native_motion::Layout motionLayout;
bedrock::CameraRig cameraRig;
std::mutex borionMutex;
std::atomic<bool> suppressMovement{}, packetReady{};
std::atomic<void *> observedSender{};
std::atomic<unsigned> movementPacketsSeen{}, blockedPackets{};
BDS_HookToken packetToken{}, wireToken{};
void *subscribedPacketTarget{};
std::atomic<void *> capturedSender{}, capturedSendTarget{};
std::atomic<std::uint64_t> capturedSendEpoch{};
std::size_t statePositionOffset{}, statePreviousOffset{}, stateVelocityOffset{}, fallOffset{};
bedrock::NativePacketSender senderLayout;
bool renderReady{};
std::uint32_t fallHash{};
std::uintptr_t moduleTextBegin{}, moduleTextEnd{};

bool inText(void *pointer) noexcept {
    const auto p = reinterpret_cast<std::uintptr_t>(pointer);
    return p >= moduleTextBegin && p + 16 >= p && p + 16 < moduleTextEnd;
}

bedrock::PacketWireEvents packetEvents;
bedrock::MovementPackets movementPackets;
struct PacketPose {
    Vec position{};
    Clock::time_point seen{};
    std::uint64_t runtime{};
    std::uint8_t onGround{};
};
struct MovementAnchor {
    std::uint64_t epoch{}, runtime{};
    std::array<PacketPose, 2> poses{};
};
// Serialize only scalar packet edits and activation/restoration phases. Never
// hold this across the native send continuation or another network operation.
std::mutex packetMotionMutex, packetPoseMutex;
MovementAnchor observedPacketPoses;
std::atomic<std::shared_ptr<const MovementAnchor>> movementAnchor;
std::atomic<unsigned> anchoredPackets{}, interactionPackets{};
std::atomic<unsigned> wirePacketsSeen{}, wireMovementRejected{}, wireWithoutSender{};

void observeMotionPose(const bedrock::WirePacket &packet, bool auth) noexcept {
    bedrock::MovementPacketPose value;
    if (!movementPackets.pose(packet, value)) {
        wireMovementRejected.fetch_add(1);
        return;
    }
    PacketPose pose{value.position, Clock::now(), value.runtimeId, value.onGround};
    const auto e = epoch.load();
    std::unique_lock lock(packetPoseMutex, std::try_to_lock);
    if (!lock)
        return;
    if (observedPacketPoses.epoch != e) {
        observedPacketPoses = {};
        observedPacketPoses.epoch = e;
    }
    observedPacketPoses.poses[auth ? 0 : 1] = pose;
    movementPacketsSeen.fetch_or(auth ? 2u : 1u);
}
bool prepareMovementAnchor(std::uint64_t runtime, Vec position) {
    MovementAnchor anchor;
    {
        std::unique_lock lock(packetPoseMutex, std::try_to_lock);
        if (!lock)
            return false;
        anchor = observedPacketPoses;
    }
    if (anchor.epoch != epoch.load())
        return false;
    bool ready = false;
    const auto now = Clock::now();
    for (std::size_t i = 0; i < anchor.poses.size(); ++i) {
        auto &pose = anchor.poses[i];
        const float dx = pose.position.x - position.x, dy = pose.position.y - position.y,
                    dz = pose.position.z - position.z;
        if (pose.seen == Clock::time_point{} || now - pose.seen > std::chrono::milliseconds(500) ||
            dx * dx + dy * dy + dz * dz > 16.f || (i == 1 && pose.runtime != runtime)) {
            pose = {};
            continue;
        }
        ready = true;
    }
    if (!ready)
        return false;
    anchor.runtime = runtime;
    movementAnchor.store(std::make_shared<const MovementAnchor>(anchor));
    return true;
}
void packetEvent(bedrock::PacketWireSendEvent &event,
                 const std::shared_ptr<const MovementAnchor> &enteringAnchor) noexcept {
    auto &packet = event.packet();
    if (!bedrock::MovementPackets::handles(event.id()))
        return;
    const bool auth = event.id() == bedrock::PacketId::PlayerAuthInput;
    std::unique_lock phase(packetMotionMutex);
    const auto anchor = enteringAnchor ? enteringAnchor : movementAnchor.load();
    if (!anchor) {
        observeMotionPose(packet, auth);
        return;
    }
    const auto &saved = anchor->poses[auth ? 0 : 1];
    if (anchor->epoch != epoch.load() || saved.seen == Clock::time_point{}) {
        disable(ReadFailed);
        blockedPackets.fetch_add(1);
        event.cancel();
        return;
    }
    const auto result = movementPackets.anchor(
        packet, {saved.position, saved.runtime, saved.onGround}, anchor->runtime);
    if (!result.applied) {
        disable(ReadFailed);
        blockedPackets.fetch_add(1);
        event.cancel();
        return;
    }
    if (result.interaction)
        interactionPackets.fetch_add(1);
    anchoredPackets.fetch_add(1);
}
void BDS_HOOK_CALL packetSubscriber(void *, void *raw, BDS_HookNext next, void *frame) noexcept {
    // Runtime 4.6 attributes synchronous wire events to this native sender.
    next(frame, raw);
}
void BDS_HOOK_CALL wireSubscriber(void *, void *raw, BDS_HookNext next, void *frame) noexcept {
    auto &args = *static_cast<BDS_PacketWireArgs *>(raw);
    if (!args.sender) {
        wireWithoutSender.fetch_add(1);
        next(frame, raw);
        return;
    }
    if (args.sender != observedSender.load()) {
        next(frame, raw);
        return;
    }
    wirePacketsSeen.fetch_add(1);
    const auto enteringAnchor = movementAnchor.load();
    if (!packetEvents.dispatch(
            args, [&](bedrock::PacketWireSendEvent &event) { packetEvent(event, enteringAnchor); },
            next, frame)) {
        if (suppressMovement.load()) {
            disable(ReadFailed);
            blockedPackets.fetch_add(1);
        } else
            next(frame, raw);
    }
}
void observePacketSender(void *instance) noexcept {
    if (stop.load())
        return;
    void *sender{};
    void *target{};
    if (!senderLayout.resolve(instance, sender, target) || !inText(target))
        return;
    if (packetReady.load() && sender != observedSender.load()) {
        disable();
        packetReady.store(false);
        movementPacketsSeen.store(0);
    }
    capturedSender.store(sender);
    capturedSendTarget.store(target);
    capturedSendEpoch.store(epoch.load());
}
bool installPacketSource() {
    if (packetReady.load())
        return true;
    const auto capturedEpoch = capturedSendEpoch.load();
    void *sender = capturedSender.load();
    void *target = capturedSendTarget.load();
    if (capturedEpoch != epoch.load() || !sender || !inText(target))
        return false;
    if (!BDS_HookOwnsTarget(BDS_HOOK_PACKET_SEND_TO_SERVER, target)) {
        DWORD64 image{};
        const auto *entry =
            RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(target), &image, nullptr);
        if (!entry || image + entry->BeginAddress != reinterpret_cast<DWORD64>(target))
            return false;
    }
    if (!wireToken && !packetEvents.subscribe(wireSubscriber, nullptr, &wireToken))
        return false;
    if (packetToken) {
        if (target != subscribedPacketTarget)
            return false;
        observedSender.store(sender);
        packetReady.store(capturedEpoch == epoch.load());
        return packetReady.load();
    }
    observedSender.store(sender);
    if (!BDS_HookSubscribe(BDS_HOOK_PACKET_SEND_TO_SERVER, target, packetSubscriber, nullptr,
                           &packetToken))
        return false;
    subscribedPacketTarget = target;
    packetReady.store(capturedEpoch == epoch.load());
    log("Freecam wire PacketHook ready [Packet ID, pre-batch read/write]; sender RVA=" +
        std::to_string(reinterpret_cast<std::uintptr_t>(target) -
                       reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))) +
        " [runtime-derived vtable slot; candidate]");
    return true;
}
std::uint64_t actorRuntime(void *actor) noexcept {
    std::uint64_t id{};
    read(component(actor, actors.runtimeIdComponentHash), actors.runtimeIdValueOffset, id);
    return id;
}
bool restoreBorionLocked() noexcept {
    std::lock_guard phase(packetMotionMutex);
    if (!borion.saved)
        return true;
    void *registry{};
    std::uint32_t entity{};
    if (borion.epoch != epoch.load() || borion.actor != local.load() ||
        actorRuntime(borion.actor) != borion.runtime ||
        !read(borion.actor, actors.actorEntityContextOffset + actors.contextRegistryOffset,
              registry) ||
        registry != borion.registry ||
        !read(borion.actor, actors.actorEntityContextOffset + actors.contextEntityIdOffset,
              entity) ||
        entity != borion.entity) {
        cameraRig.reset();
        borion.saved = false;
        suppressMovement.store(false);
        movementAnchor.store(nullptr);
        active.store(false);
        return true;
    }
    auto *state = component(borion.actor, actors.stateVectorComponentHash);
    auto *shape = component(borion.actor, actors.aabbShapeComponentHash);
    auto *fall = component(borion.actor, fallHash);
    if (!state || !shape || !fall)
        return false;
    // Position restore is a teleport, not a frame-long interpolation across
    // the flight path. Borion also clears motion/fall distance when returning.
    const bedrock::native_motion::Snapshot restored{borion.oldPosition, borion.oldPosition, Vec{},
                                                    borion.oldBounds, 0.f};
    const bool ok =
        bedrock::native_motion::apply(borion.actor, actors, motionLayout, component, restored);
    if (ok) {
        cameraRig.reset();
        borion.saved = false;
        suppressMovement.store(false);
        movementAnchor.store(nullptr);
        active.store(false);
    }
    return ok;
}
void restoreBorionBeforeBoundary() noexcept {
    std::unique_lock guard(borionMutex, std::try_to_lock);
    if (guard.owns_lock())
        restoreBorionLocked();
}
void borionTick(const bedrock::LevelTickEvent &) {
    std::unique_lock guard(borionMutex, std::try_to_lock);
    if (!guard.owns_lock())
        return;
    const auto c = config.load();
    if (!c->enabled && !borion.saved) {
        if (desired.load()) disable();
        return;
    }
    observePacketSender(client.load());
    auto cameraScope = nativeCamera.tick(client.load(), bedrock::WorldEpoch{epoch.load()});
    void *player = cameraScope ? nativeCamera.localActor() : nullptr;
    local.store(player);
    auto typeScope = gameTypes.tick(client.load(), bedrock::WorldEpoch{epoch.load()});
    if ((c->spectatorMode || cameraVisual.leaseActive()) && gameplay.load() && player) {
        if (const auto type = gameTypes.gameType())
            cameraVisual.noteNativeModeContext(
                (epoch.load() << 8) | static_cast<std::uint64_t>(static_cast<int>(*type) + 1));
    }
    if (c->visibilityDiagnostics && !stop.load()) {
        static Clock::time_point next{};
        const auto now = Clock::now();
        if (now >= next) {
            next = now + std::chrono::milliseconds(500);
            int type = -1;
            if (gameplay.load() && player) {
                if (const auto current = gameTypes.gameType())
                    type = static_cast<int>(*current);
            }
            cameraVisual.setDiagnosticGameType(
                type, epoch.load(),
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())
                        .count()));
        }
    }
    if (borion.saved &&
        (restoreRequested.load() || stop.load() || !desired.load() || !gameplay.load() ||
         !c->enabled || player != borion.actor || borion.epoch != epoch.load())) {
        if (!restoreBorionLocked())
            note(ReadFailed);
        else
            restoreRequested.store(false);
        return;
    }
    if (!borion.saved)
        restoreRequested.store(false);
    if (c->visibilityDiagnostics && !stop.load() && gameplay.load()) {
        static Clock::time_point next{};
        if (Clock::now() >= next) {
            next = Clock::now() + std::chrono::seconds(5);
            const auto sample = nativeCamera.readPlayerProxy();
            if (sample)
                log(nlohmann::json(
                        {{"event", "freecam-player-pose"},
                         {"runtime_id", sample->runtimeId},
                         {"position", {sample->position.x, sample->position.y, sample->position.z}},
                         {"velocity", {sample->velocity.x, sample->velocity.y, sample->velocity.z}},
                         {"fall_distance", sample->fallDistance},
                         {"sender_ready", packetReady.load()},
                         {"movement_packet_kinds", movementPacketsSeen.load()},
                         {"wire_packets", wirePacketsSeen.load()},
                         {"wire_movement_rejected", wireMovementRejected.load()},
                         {"wire_without_sender", wireWithoutSender.load()},
                         {"wire_bridge_runtime_verified", false}})
                        .dump());
            else
                log("Freecam waiting: local Actor / StateVector / AABB / rotation / FallDistance "
                    "snapshot unavailable");
        }
    }
    if (stop.load() || !desired.load() || !gameplay.load() || !c->enabled || !player)
        return;
    if (!packetReady.load() || !movementPacketsSeen.load()) {
        note(Waiting);
        return;
    }
    const auto pose = nativeCamera.readPlayerProxy();
    int hurt{};
    if (!cameraScope || !pose || (c->disableOnDamage && !hurtTime(player, hurt))) {
        disable(ReadFailed);
        restoreBorionLocked();
        return;
    }
    const auto position = pose->position, previous = pose->previous, velocity = pose->velocity;
    const auto bounds = pose->bounds;
    const float fallDistance = pose->fallDistance, yaw = pose->rotation.yaw;
    if (c->disableOnDamage && hurt > 0) {
        disable(Damage);
        restoreBorionLocked();
        return;
    }
    if (!borion.saved) {
        const auto runtime = actorRuntime(player);
        if (!runtime || !bounds.valid()) {
            disable(ReadFailed);
            return;
        }
        void *registry{};
        std::uint32_t entity{};
        if (!read(player, actors.actorEntityContextOffset + actors.contextRegistryOffset,
                  registry) ||
            !read(player, actors.actorEntityContextOffset + actors.contextEntityIdOffset, entity)) {
            disable(ReadFailed);
            return;
        }
        std::unique_lock phase(packetMotionMutex);
        if (!prepareMovementAnchor(runtime, position)) {
            note(Waiting);
            return;
        }
        borion = {true,     player,   epoch.load(), runtime,  registry, entity,
                  position, previous, velocity,     position, bounds,   fallDistance};
        if (!cameraRig.begin(position, bounds, runtime, borion.epoch)) {
            disable(ReadFailed);
            restoreBorionLocked();
            return;
        }
        suppressMovement.store(true);
        flightSerial.fetch_add(1);
        active.store(true);
        note(Activated);
    }
    if (actorRuntime(player) != borion.runtime) {
        disable(ReadFailed);
        restoreBorionLocked();
        return;
    }
    const auto down = [](unsigned key) { return keys[key].load() ? 1.f : 0.f; };
    const float forward = down(c->forward) - down(c->back), side = down(c->right) - down(c->left);
    const float speed = c->speed / 20.f * (keys[c->fast].load() ? c->fastMultiplier : 1.f);
    const auto cameraFrame = cameraRig.advance(
        bedrock::Yaw{yaw}, {forward, side, down(c->up) - down(c->down)}, {speed * 20.f, 10.f},
        std::chrono::duration<float>{.05f}, borion.runtime, epoch.load());
    if (!cameraFrame) {
        disable(ReadFailed);
        restoreBorionLocked();
        return;
    }
    borion.position = cameraFrame->position;
    if (!finite(borion.position)) {
        disable(ReadFailed);
        restoreBorionLocked();
        return;
    }
    if (nativeCamera.applyPlayerProxy(*cameraFrame, borion.runtime,
                                      bedrock::WorldEpoch{epoch.load()}) !=
        bedrock::CameraChange::Applied) {
        disable(ReadFailed);
        restoreBorionLocked();
        return;
    }
    if (renderReady && c->playerHitboxes) {
        Capture batch;
        batch.epoch = epoch.load();
        batch.origin = borion.position;
        if (playerBounds.capture(player, &batch, c->captureRows)) {
            std::unique_lock lock(mailboxMutex, std::try_to_lock);
            if (lock.owns_lock()) {
                mailbox = batch;
                mailboxReady = true;
            }
        }
    }
}

void BDS_HOOK_CALL renderSubscriber(void *, void *raw, BDS_HookNext next, void *frame) {
    auto &args = *static_cast<BDS_RenderLevelArgs *>(raw);
    if (auto* camera = outline.renderPlayer(args.renderer)) cameraVisual.notePrimaryCamera(camera);
    cameraVisual.updateTerrain(args.renderer);
    if (!cameraVisual.observeReady())
        cameraVisual.captureFromRenderer(args.renderer);
    next(frame, raw);
    if (!active.load() || stop.load() || !config.load()->playerHitboxes)
        return;
    static thread_local bool drawing = false;
    if (drawing)
        return;
    const auto geometry = mesh.load();
    if (!geometry || geometry->epoch != epoch.load() || !geometry->count ||
        Clock::now() - geometry->made > std::chrono::milliseconds(250))
        return;
    drawing = true;
    try {
        static thread_local bedrock::WorldOutlineGeometry frameGeometry;
        if (frameGeometry.lines.empty()) frameGeometry.lines.emplace_back();
        auto& lines = frameGeometry.lines.front();
        lines.color = {.3f, .95f, .5f, 1.f};
        lines.vertices.resize(geometry->count);
        const float alpha = std::clamp(
            std::chrono::duration<float>(Clock::now() - geometry->made).count() / .05f, 0.f, 1.f);
        for (unsigned i = 0; i < geometry->count; ++i) {
            const auto a = geometry->fromVertices[i], b = geometry->vertices[i];
            lines.vertices[i] = {a.x + (b.x - a.x) * alpha, a.y + (b.y - a.y) * alpha,
                                 a.z + (b.z - a.z) * alpha};
        }
        if (outline.draw(args.renderer, args.screen, frameGeometry) == bedrock::OutlineDraw::Fault)
            disable(RenderFault);
    } catch (...) {
        disable(RenderFault);
    }
    drawing = false;
}
using bedrock::native_profile::number;
void prepareProfile(const std::string &profile) {
    const auto j = bedrock::parseNativeProfile(profile);
    const auto &f = j.at("native_camera");
    if (f.at("schema") != 1 || f.at("pointer_size") != 8)
        throw std::runtime_error("unsupported Freecam layout");
    hurtHash = number(f.at("hurt_time_component_hash"));
    hurtOffset = number(f.at("hurt_time_value_offset"));
    if (hurtOffset > 4096 || f.at("hurt_time_value_size") != 4)
        throw std::runtime_error("invalid camera/hurt layout");
    const auto &visual = f.at("visual");
    if (visual.value("revision", 0) < f.value("minimum_visual_revision", 0))
        throw std::runtime_error("Freecam visual definition predates InsideBlockEffect port; exact "
                                 "embedded profile required");
    if (visual.at("schema") != 1)
        throw std::runtime_error("unsupported camera visual schema");
    motionLayout = bedrock::native_motion::Layout::fromProfile(profile);
    statePositionOffset =
        number(j.at("native_motion").at("state_vector").at("state_position_offset"));
    statePreviousOffset = motionLayout.previous;
    stateVelocityOffset = motionLayout.velocity;
    fallHash = motionLayout.fallHash;
    fallOffset = motionLayout.fall;
    senderLayout.load(j);
    packetEvents.load(j);
    if (statePositionOffset != 0 || statePreviousOffset != 12 || stateVelocityOffset != 24 ||
        fallOffset != 0)
        throw std::runtime_error("unsupported Borion native actor layout");

    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    moduleTextBegin = base + number(j.at("target").at("text_rva"));
    moduleTextEnd = moduleTextBegin + number(j.at("target").at("text_size"));
    std::string error;
    if (!bedrock::parseNativeActorLayout(profile, actors, error))
        throw std::runtime_error(error);
    actorComponents.configure(actors,j);
    renderReady = bedrock::parseWorldRenderProfile(profile, render, error);
    if(renderReady)playerBounds.configure(actors,render);
}
} // namespace freecam_detail

void onFreecamKeybind(bool pressed) {
    using namespace freecam_detail;
    if (stop.load() || !gameplay.load()) return;
    const auto c = config.load();
    if (!c->enabled) return;
    if (!pressed) return;
    if (desired.load()) {
        disable();
    } else if (!restoreRequested.load()) {
        releaseMovement(*c);
        desired.store(true);
    }
}

void setFreecamConfig(const freecam::Config& next) {
    freecam_detail::config.store(std::make_shared<const freecam::Config>(next));
}

void runFreecam(const bedrock::RuntimeProfile& selected, const std::filesystem::path& directory) {
    using namespace freecam_detail;
    try {
        logFile.open(directory / L"DynamicClient-Freecam.log", std::ios::trunc);
        std::string error;
        const bedrock::ProfileRequirements required{
            bedrock::Feature::Input,      bedrock::Feature::Gameplay,
            bedrock::Feature::LevelTick,  bedrock::Feature::Camera,
            bedrock::Feature::Motion,     bedrock::Feature::PacketSender,
            bedrock::Feature::PacketWire, bedrock::Feature::WorldEvents};
        if (!selected.supports(required, error)) throw std::runtime_error(error);
        const auto &profile = selected.json();
        const auto &resolver = selected.resolver();
        prepareProfile(profile);
        if (!nativeCamera.load(selected, error)) throw std::runtime_error(error);
        if (!gameTypes.load(selected, error))
            log("Freecam native game type observation unavailable: " + error);
        const auto j = bedrock::parseNativeProfile(profile);
        const auto &f = j.at("native_camera");
        visualRevision = f.at("visual").value("revision", 9);
        profileGameVersion = j.at("target").at("version").get<std::string>();
        profileExeSha = j.at("target").at("sha256").get<std::string>();
        log("Freecam profile source=" +
            std::string(bedrock::profileSourceName(selected.choice().source)) +
            " version=" + selected.choice().version + " path=" + selected.choice().path +
            " exe_sha256=" + profileExeSha + " visual_revision=" + std::to_string(visualRevision));
        // Include selection/load errors even when an old owner's capabilities
        // stop startup before the normal ready message.
        log(BDS_HookRuntimeDiagnostic());
        if (!BDS_HookRuntimeSupports(BDS_HOOK_PACKET_SEND_TO_SERVER))
            throw std::runtime_error(
                "active runtime lacks packet send capability; restart with Runtime 4.2 DLL");
        if (renderReady && !outline.load(selected, error)) {
            renderReady = false;
            log("Freecam native drawing unavailable: " + error);
        }
        const bedrock::NativeCameraVisual::Policy visualPolicy{
            []() noexcept { return active.load(); },
            []() noexcept { return stop.load(); },
            []() noexcept { return config.load()->spectatorMode; },
            []() noexcept { return config.load()->visibilityDiagnostics; },
            []() noexcept { return epoch.load(); },
            []() noexcept { return flightSerial.load(); }};
        std::string visualError;
        if (!cameraVisual.load(j, render.player, render.origin, visualPolicy, visualError))
            log("Freecam camera visual unavailable: " + visualError);
        std::string terrainError;
        if (cameraVisual.installTerrain(renderReady, terrainError))
            log("Freecam spectator culler ready [native selector hook; profile-defined vtable; game "
                "mode unchanged; visual revision9]");
        else
            log("Freecam terrain culling unavailable: " + terrainError);
        std::string observeError;
        if (cameraVisual.installRenderObserve(renderReady, observeError))
            log("Freecam native ViewRenderData observer ready [read only; no overlay "
                "suppression]");
        else
            log("Freecam ViewRenderData observer unavailable: " + observeError);
        std::string createViewError;
        if (cameraVisual.installCreateView(createViewError))
            log("Freecam view factory ready [new output draw_inside_cubes=false; no "
                "game-mode/ability writes; visual revision9]");
        else
            log("Freecam view factory unavailable: " + createViewError);
        if (insideBlockEffect.install(selected, bedrock::generated::kSymbolDatabase,
                                      &insideBlockEffectPolicy, error))
            log("Freecam RenderDragon InsideBlockEffect filter ready [SDK tag + paired extraction "
                "caller; versioned visual profile]");
        else
            log("Freecam RenderDragon InsideBlockEffect filter unavailable: " + error);
        log("Freecam legacy inside-cube bypass retired [revision7 did not resolve underground "
            "display]");
        api.on<bedrock::KeyEvent>(keyEvent);
        api.on<bedrock::MouseEvent>(mouseEvent);
        api.on<bedrock::GameplayEvent>([](const bedrock::GameplayEvent &e) {
            api.setWorldEventClient(e.clientInstance);
            client.store(e.clientInstance);
            gameplay.store(e.active);
            if (!e.active) {
                disable();
                for (auto &key : keys)
                    key.store(false);
            }
        });
        api.on<bedrock::WorldChangeEvent>([](const bedrock::WorldChangeEvent &event) {
            if (event.reason == bedrock::WorldChangeReason::Left ||
                event.reason == bedrock::WorldChangeReason::JoinCancelled)
                restoreBorionBeforeBoundary();
            // Post-transition notifications must never restore old coordinates
            // into the new world. The next tick discards the expired epoch.
            reset();
            packetReady.store(false);
            observedSender.store(nullptr);
            movementPacketsSeen.store(0);
            capturedSendEpoch.store(0);
        });
        api.on<bedrock::LevelTickEvent>(borionTick);
        hooks = std::make_unique<bedrock::GameHooks>(api);
        if (!hooks->installInput(resolver, error) || !hooks->installGameplay(resolver, error) ||
            !hooks->installLevelTick(resolver, error) ||
            !hooks->installWorldEvents(resolver, profile, error))
            throw std::runtime_error(error);
        if (renderReady && !BDS_HookSubscribe(BDS_HOOK_RENDER_LEVEL, outline.renderTarget(), renderSubscriber,
                                              nullptr, &renderToken))
            throw std::runtime_error("render hook conflict/install failure; restart Minecraft with "
                                     "matching runtime DLLs");
        if (!renderReady)
            log("Freecam player hitboxes unavailable: exact-build native drawing definitions are "
                "pending; flight core remains enabled");
        log("ready " + j.at("target").at("version").get<std::string>() +
            "; Borion Freecam candidate; press Esc and return to gameplay, then configured toggle "
            "key (default M); compatible runtime");
        log(std::string("active runtime owner: ") + BDS_HookRuntimeVersion());
        worker(directory);
    } catch (const std::exception &e) {
        log(std::string("startup failed: ") + e.what());
    }
    disable();
    // Keep the render subscriber pinned: a later frame restores an owned culler.
    // Do not tear down trampolines while a native thread may still enter them.
    // Keep pinned callbacks as no-ops/pass-through after stop, so the next
    // owned game tick can restore an outstanding proxy before movement resumes.
    stop.store(true);
}
} // namespace dynamic_client

#endif
