#include <bedrock/sdk.hpp>
#include <bedrock/sdk/keybinds.hpp>
#include <bedrock/sdk/game_hooks.hpp>
#include <bedrock/sdk/camera_control.hpp>
#include <bedrock/sdk/movement_packets.hpp>
#include <bedrock/sdk/packet_wire.hpp>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include "client.hpp"
#include "worker.hpp"
#include "freecam/config.hpp"
#include <nlohmann/json.hpp>
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
bedrock::sdk::Hooks hooks;
bedrock::sdk::CameraControl cameraControl;
bedrock::sdk::PacketWire packetWire;
bedrock::sdk::MovementPackets movementPackets;
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
bool insideBlockEffectPolicy() noexcept {
    return active.load() && !stop.load() && gameplay.load() &&
           cameraControl.primaryCameraEpoch() == epoch.load() && config.load()->spectatorMode;
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
    cameraControl.reset();
    epoch.fetch_add(1);
    local.store(nullptr);
    for (auto &key : keys)
        key.store(false);
}
bool hurtTime(void *actor, int &value) noexcept {
    return cameraControl.hurtTime(actor, value);
}
std::uint64_t actorRuntime(void *actor) noexcept {
    return cameraControl.actorRuntimeId(actor).value_or(0);
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
        hooks.sendKeyboardFeed(key, false);
    }
    (void)hooks.sendMouseInput(1, false);
    (void)hooks.sendMouseInput(2, false);
    releasing = false;
}
void keyEvent(bedrock::sdk::KeyEventValue &e) {
    if (releasing || stop.load())
        return;
    const auto c = config.load();
    const unsigned k = static_cast<unsigned>(e.key);
    if (k < keys.size())
        keys[k].store(e.pressed);
    if (k == boundKey("freecam", bedrock::sdk::Key::M)) {
        // Toggle is owned by the SDK keybind (DynamicClientMod::onKeybind).
        // Do NOT cancel here: cancelling the keyboard feed skips its original
        // handler, which is what drives the downstream native keyboard event
        // that the SDK keybind dispatcher depends on. Cancelling therefore
        // starved onKeybind("freecam") and made M dead (P4C-R3 regression).
        // M is unbound in vanilla, so letting the feed through is harmless.
        return;
    }
    if (desired.load() && isMovement(k, *c))
        e.cancelled = true;
}
void mouseEvent(bedrock::sdk::MouseEventValue &e) {
    if (releasing || stop.load() || !desired.load())
        return;
    const auto c = config.load();
    if (e.action == 0)
        return;
    else if (!c->allowInteraction &&
             (e.action == 1 || e.action == 2 || e.action == 3 || e.action == 4))
        e.cancelled = true;
}
// Borion-style local Player proxy. All mutable native access is confined to
// LevelTick/world teardown callbacks; the network middleware reads atomics only.
struct BorionState {
    bool saved{};
    void *actor{};
    std::uint64_t epoch{}, runtime{};
    std::uintptr_t registry{};
    std::uint32_t entity{};
    Vec oldPosition{}, oldPrevious{}, oldVelocity{}, position{};
    bedrock::sdk::ActorBoundsValue oldBounds{};
    float oldFall{};
};
BorionState borion;
std::mutex borionMutex;
std::atomic<bool> suppressMovement{}, packetReady{};
std::atomic<void *> observedSender{};
std::atomic<unsigned> movementPacketsSeen{}, blockedPackets{};
std::atomic<void *> capturedSender{}, capturedSendTarget{};
std::atomic<std::uint64_t> capturedSendEpoch{};
void *installedPacketTarget{};

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

void observeMotionPose(const std::string &bytes, int packetId, bool auth) noexcept {
    const auto value = movementPackets.pose(bytes, packetId);
    if (!value) {
        wireMovementRejected.fetch_add(1);
        return;
    }
    PacketPose pose{{value->position_x, value->position_y, value->position_z},
                    Clock::now(), value->runtimeId, value->onGround};
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
void packetEvent(bedrock::sdk::PacketWireEvent &event,
                 const std::shared_ptr<const MovementAnchor> &enteringAnchor) noexcept {
    if (!movementPackets.handles(event.packetId))
        return;
    const bool auth = movementPackets.isAuthInput(event.packetId);
    std::unique_lock phase(packetMotionMutex);
    const auto anchor = enteringAnchor ? enteringAnchor : movementAnchor.load();
    if (!anchor) {
        observeMotionPose(event.bytes, event.packetId, auth);
        return;
    }
    const auto &saved = anchor->poses[auth ? 0 : 1];
    if (anchor->epoch != epoch.load() || saved.seen == Clock::time_point{}) {
        disable(ReadFailed);
        blockedPackets.fetch_add(1);
        event.cancelled = true;
        return;
    }
    bedrock::sdk::MovementPacketPose pose;
    pose.runtimeId = saved.runtime;
    pose.onGround = saved.onGround;
    pose.position_x = saved.position.x;
    pose.position_y = saved.position.y;
    pose.position_z = saved.position.z;
    const auto result = movementPackets.anchor(event.bytes, event.packetId, pose, anchor->runtime);
    if (!result.applied) {
        disable(ReadFailed);
        blockedPackets.fetch_add(1);
        event.cancelled = true;
        return;
    }
    if (result.interaction)
        interactionPackets.fetch_add(1);
    anchoredPackets.fetch_add(1);
}
void wirePacketHandler(bedrock::sdk::PacketWireEvent &event, void *) noexcept {
    // The wire bridge fires for every sender; only the observed local
    // sender's packets are freecam's business (the native subscriber
    // returned before dispatch for any other sender).
    if (!event.sender) {
        wireWithoutSender.fetch_add(1);
        return;
    }
    if (event.sender != observedSender.load())
        return;
    if (!event.readable) {
        // The frame could not be copied safely. Mirror the native policy:
        // while movement is suppressed an unparsable movement frame must
        // not leak through; otherwise forward it untouched.
        if (suppressMovement.load()) {
            disable(ReadFailed);
            blockedPackets.fetch_add(1);
            event.cancelled = true;
        }
        return;
    }
    wirePacketsSeen.fetch_add(1);
    const auto enteringAnchor = movementAnchor.load();
    packetEvent(event, enteringAnchor);
}
void observePacketSender(void *instance) noexcept {
    if (stop.load())
        return;
    void *sender{};
    void *target{};
    if (!cameraControl.resolveSender(instance, sender, target) || !packetWire.accepts(target))
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
    if (capturedEpoch != epoch.load() || !sender || !packetWire.accepts(target))
        return false;
    if (packetWire.installed()) {
        if (target != installedPacketTarget)
            return false;
        observedSender.store(sender);
        packetReady.store(capturedEpoch == epoch.load());
        return packetReady.load();
    }
    observedSender.store(sender);
    std::string error;
    if (!packetWire.install(target, &wirePacketHandler, nullptr, error))
        return false;
    installedPacketTarget = target;
    packetReady.store(capturedEpoch == epoch.load());
    log("Freecam wire PacketHook ready [Packet ID, pre-batch read/write]; sender RVA=" +
        std::to_string(reinterpret_cast<std::uintptr_t>(target) -
                       reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))) +
        " [runtime-derived vtable slot; candidate]");
    return true;
}
bool restoreBorionLocked() noexcept {
    std::lock_guard phase(packetMotionMutex);
    if (!borion.saved)
        return true;
    std::uintptr_t registry{};
    std::uint32_t entity{};
    if (borion.epoch != epoch.load() || borion.actor != local.load() ||
        actorRuntime(borion.actor) != borion.runtime ||
        !cameraControl.actorIdentity(borion.actor, registry, entity) ||
        registry != borion.registry ||
        entity != borion.entity) {
        cameraControl.resetCamera();
        borion.saved = false;
        suppressMovement.store(false);
        movementAnchor.store(nullptr);
        active.store(false);
        return true;
    }
    // Position restore is a teleport, not a frame-long interpolation across
    // the flight path. Borion also clears motion/fall distance when returning
    // (restoreActorMotion applies exactly that snapshot natively).
    const bool ok =
        cameraControl.restoreActorMotion(borion.actor, borion.oldPosition, borion.oldBounds);
    if (ok) {
        cameraControl.resetCamera();
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
void borionTick(const bedrock::sdk::LevelTickEventValue &) {
    std::unique_lock guard(borionMutex, std::try_to_lock);
    if (!guard.owns_lock())
        return;
    const auto c = config.load();
    if (!c->enabled && !borion.saved) {
        if (desired.load()) disable();
        return;
    }
    observePacketSender(client.load());
    const bool cameraTick = cameraControl.tick(client.load(), epoch.load());
    void *player = cameraTick ? cameraControl.localActor() : nullptr;
    local.store(player);
    const auto gameType = cameraControl.gameType();
    if ((c->spectatorMode || cameraControl.leaseActive()) && gameplay.load() && player) {
        if (gameType.available)
            cameraControl.noteNativeModeContext(
                (epoch.load() << 8) | static_cast<std::uint64_t>(gameType.value + 1));
    }
    if (c->visibilityDiagnostics && !stop.load()) {
        static Clock::time_point next{};
        const auto now = Clock::now();
        if (now >= next) {
            next = now + std::chrono::milliseconds(500);
            int type = -1;
            if (gameplay.load() && player && gameType.available) {
                type = gameType.value;
            }
            cameraControl.setDiagnosticGameType(
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
            const auto sample = cameraControl.readPlayerProxy();
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
    const auto pose = cameraControl.readPlayerProxy();
    int hurt{};
    if (!cameraTick || !pose || (c->disableOnDamage && !hurtTime(player, hurt))) {
        disable(ReadFailed);
        restoreBorionLocked();
        return;
    }
    const auto position = pose->position, previous = pose->previous, velocity = pose->velocity;
    const auto bounds = pose->bounds;
    const float fallDistance = pose->fallDistance, yaw = pose->rotation_yaw;
    if (c->disableOnDamage && hurt > 0) {
        disable(Damage);
        restoreBorionLocked();
        return;
    }
    if (!borion.saved) {
        const auto runtime = actorRuntime(player);
        if (!runtime || !bounds.valid) {
            disable(ReadFailed);
            return;
        }
        std::uintptr_t registry{};
        std::uint32_t entity{};
        if (!cameraControl.actorIdentity(player, registry, entity)) {
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
        if (!cameraControl.beginCamera(position, bounds, runtime, borion.epoch)) {
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
    const auto cameraFrame = cameraControl.advanceCamera(
        yaw, {forward, side, down(c->up) - down(c->down)}, {speed * 20.f, 10.f},
        .05f, borion.runtime, epoch.load());
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
    if (cameraControl.applyPlayerProxy(*cameraFrame, borion.runtime, epoch.load()) !=
        bedrock::sdk::CameraChange::Applied) {
        disable(ReadFailed);
        restoreBorionLocked();
        return;
    }
    if (cameraControl.renderReady() && c->playerHitboxes) {
        Capture batch;
        batch.origin = borion.position;
        if (cameraControl.capturePlayerBounds(player, epoch.load(), c->captureRows, batch)) {
            std::unique_lock lock(mailboxMutex, std::try_to_lock);
            if (lock.owns_lock()) {
                mailbox = batch;
                mailboxReady = true;
            }
        }
    }
}

// Render-thread drawing of the worker-built player box lines. The facade's
// own render subscription runs the camera-visual bookkeeping (primary
// camera, terrain, view capture) before this handler is invoked.
void drawPlayerBoxes(bedrock::sdk::CameraRenderFrame &frame, void *) {
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
        const float alpha = std::clamp(
            std::chrono::duration<float>(Clock::now() - geometry->made).count() / .05f, 0.f, 1.f);
        std::array<bedrock::sdk::CameraLineSegment, 128 * 12> segments{};
        std::size_t segmentCount = 0;
        for (unsigned i = 0; i + 1 < geometry->count; i += 2) {
            const auto a0 = geometry->fromVertices[i], b0 = geometry->vertices[i];
            const auto a1 = geometry->fromVertices[i + 1], b1 = geometry->vertices[i + 1];
            segments[segmentCount++] = {
                {a0.x + (b0.x - a0.x) * alpha, a0.y + (b0.y - a0.y) * alpha,
                 a0.z + (b0.z - a0.z) * alpha},
                {a1.x + (b1.x - a1.x) * alpha, a1.y + (b1.y - a1.y) * alpha,
                 a1.z + (b1.z - a1.z) * alpha}};
        }
        if (cameraControl.drawLines(frame.renderer, frame.screen, Vec{0.f, 0.f, 0.f},
                                    segments.data(), segmentCount,
                                    {.3f, .95f, .5f, 1.f}) == bedrock::sdk::CameraDrawResult::Fault)
            disable(RenderFault);
    } catch (...) {
        disable(RenderFault);
    }
    drawing = false;
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

void runFreecam(const bedrock::sdk::Profile& profile, const bedrock::sdk::SymbolResolver& resolver,
                const std::filesystem::path& directory) {
    using namespace freecam_detail;
    try {
        logFile.open(directory / L"DynamicClient-Freecam.log", std::ios::trunc);
        std::string error;
        const auto j = nlohmann::json::parse(std::string(profile.json()));
        profileGameVersion = j.at("target").at("version").get<std::string>();
        profileExeSha = j.at("target").at("sha256").get<std::string>();
        const bedrock::sdk::CameraVisualPolicy visualPolicy{
            []() noexcept { return active.load(); },
            []() noexcept { return stop.load(); },
            []() noexcept { return config.load()->spectatorMode; },
            []() noexcept { return config.load()->visibilityDiagnostics; },
            []() noexcept { return epoch.load(); },
            []() noexcept { return flightSerial.load(); }};
        // The effect filter copies the policy pointer at install time, so it
        // must be set before configure().
        cameraControl.setEffectPolicy(&insideBlockEffectPolicy);
        std::vector<std::string> warnings;
        if (!cameraControl.configure(profile, visualPolicy, error, warnings))
            throw std::runtime_error(error);
        visualRevision = cameraControl.visualRevision();
        for (const auto& warning : warnings) log("Freecam " + warning);
        log("Freecam profile version=" + profileGameVersion +
            " exe_sha256=" + profileExeSha + " visual_revision=" + std::to_string(visualRevision));
        // Include selection/load errors even when an old owner's capabilities
        // stop startup before the normal ready message.
        log(bedrock::sdk::hookRuntimeDiagnostic());
        if (!packetWire.load(profile, error))
            throw std::runtime_error("packet wire bridge unavailable: " + error);
        log("Freecam legacy inside-cube bypass retired [revision7 did not resolve underground "
            "display]");
        hooks.onKey(keyEvent);
        hooks.onMouse(mouseEvent);
        hooks.onGameplay([](const bedrock::sdk::GameplayEventValue &e) {
            hooks.pinWorldEventClient(e.clientInstance);
            client.store(e.clientInstance);
            gameplay.store(e.active);
            if (!e.active) {
                disable();
                for (auto &key : keys)
                    key.store(false);
            }
        });
        hooks.onWorldChange([](const bedrock::sdk::WorldChangeEventValue &event) {
            if (event.reason == bedrock::sdk::WorldChangeReasonValue::Left ||
                event.reason == bedrock::sdk::WorldChangeReasonValue::JoinCancelled)
                restoreBorionBeforeBoundary();
            // Post-transition notifications must never restore old coordinates
            // into the new world. The next tick discards the expired epoch.
            reset();
            packetReady.store(false);
            observedSender.store(nullptr);
            movementPacketsSeen.store(0);
            capturedSendEpoch.store(0);
        });
        hooks.onLevelTick(borionTick);
        if (!hooks.install(resolver, profile.json(), error))
            throw std::runtime_error(error);
        if (!hooks.installInput(resolver, error))
            throw std::runtime_error(error);
        std::string renderError;
        if (!cameraControl.installRenderHook(&drawPlayerBoxes, nullptr, renderError))
            log("Freecam player hitboxes unavailable: " + renderError);
        log("ready " + profileGameVersion +
            "; Borion Freecam candidate; press Esc and return to gameplay, then configured toggle "
            "key (default M); compatible runtime");
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
