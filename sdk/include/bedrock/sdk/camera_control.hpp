#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "actors.hpp"
#include "profiles.hpp"

namespace bedrock::sdk {

// Value copy of a single camera-visual observation sample produced by the
// native render callback. Workers only ever see this copy.
struct CameraVisibilitySample {
    std::uint64_t epoch{}, milliseconds{};
    int gameType = -1;
    bool flying{}, valid{};
    float cameraPosition_x{}, cameraPosition_y{}, cameraPosition_z{};
    float origin_x{}, origin_y{}, origin_z{};
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

// Value copy of the local-player proxy state read on the owning game tick.
struct CameraPlayerSnapshot {
    std::uint64_t runtimeId{}, worldEpoch{};
    Vec3Value position{}, previous{}, velocity{};
    float fallDistance{};
    ActorBoundsValue bounds{};
    float rotation_pitch{}, rotation_yaw{};
};

// Output of advanceCamera() — the next camera position/bounds after applying
// movement input.
struct CameraFrame {
    Vec3Value position{}, previous{};
    ActorBoundsValue bounds{};
};

enum class CameraDrawResult {
    Skipped,
    Drawn,
    Fault
};

enum class CameraChange {
    Applied,
    NoPlayer,
    WrongThread,
    WorldChanged,
    InvalidValue,
    NativeFailure
};

// Game-type observation result.
struct CameraGameType {
    int value{};
    bool available{};
};

// Normalised movement input for one camera step; every component is in
// [-1, 1]. Speeds are blocks per second.
struct CameraMoveInput {
    float forward{}, right{}, up{};
};
struct CameraMoveSpeed {
    float horizontal{1.f}, vertical{.5f};
};

// One tracked Player box copied out of the native registry. `blendFrom` is the
// previous tick's box so workers can interpolate.
struct PlayerBoundsValue {
    ActorBoundsValue bounds{}, blendFrom{};
};
struct PlayerBoundsFrame {
    std::uint64_t epoch{};
    // Capture timestamp in steady-clock nanoseconds; workers rebuild a
    // time_point from it for interpolation.
    std::int64_t madeNanoseconds{};
    unsigned count{};
    std::array<PlayerBoundsValue, 128> boxes{};
    Vec3Value origin{};
};

// Inside-block-effect filter counters. `takeEffectStatistics()` exchanges the
// counters, so each sample covers the interval since the previous call.
struct CameraEffectStatistics {
    bool ready{};
    unsigned calls{}, eligible{}, skipped{}, otherCaller{};
    std::uintptr_t callerRva{};
};

// One line segment relative to a copied render origin.
struct CameraLineSegment {
    Vec3Value from{}, to{};
};

// Borrowed render-thread handles, valid only inside the render callback.
struct CameraRenderFrame {
    void* renderer{};
    void* screen{};
};
using CameraRenderHandler = void (*)(CameraRenderFrame& frame, void* user);

// Thread-safe policy consulted by the inside-block-effect filter on the render
// thread. Return true to suppress the effect for the owned caller pairs.
using CameraEffectPolicy = bool (*)() noexcept;

// Mod-supplied policy for the camera visual paths. Every callback runs on the
// render/game thread and must be lock-free and non-blocking.
struct CameraVisualPolicy {
    bool (*active)() noexcept = nullptr;
    bool (*stopped)() noexcept = nullptr;
    bool (*spectatorMode)() noexcept = nullptr;
    bool (*diagnosticsEnabled)() noexcept = nullptr;
    std::uint64_t (*epoch)() noexcept = nullptr;
    std::uint64_t (*flightSerial)() noexcept = nullptr;
};

// --------------------------------------------------------------------------
// CameraControl — public facade over the native camera rig, camera visual,
// inside-block-effect and outline renderer machinery.
//
// The header is free of native bedrock types so it compiles on Linux host
// builds; the native implementation lives in the .cpp behind #ifdef _WIN32.
// Game-thread methods are marked as such: the local-player proxy and actor
// component reads are only valid inside the owning tick/render callback.
// --------------------------------------------------------------------------
class CameraControl final {
public:
    CameraControl();
    ~CameraControl();
    CameraControl(const CameraControl&) = delete;
    CameraControl& operator=(const CameraControl&) = delete;
    CameraControl(CameraControl&&) noexcept;
    CameraControl& operator=(CameraControl&&) noexcept;

    // Parse every camera/motion/actor/render layout this facade owns from the
    // selected profile and install the visual + effect hooks. Must be called on
    // the owning game thread before any other method.
    //
    // `error` is set only for a hard failure (the flight core cannot run).
    // Capabilities that are merely unavailable for this build are appended to
    // `warnings` and reported through the ready() accessors instead.
    bool configure(const Profile& profile, const CameraVisualPolicy& policy, std::string& error,
                   std::vector<std::string>& warnings);

    // Visual definition revision from the profile (0 when unavailable).
    unsigned visualRevision() const noexcept;

    // Drop the per-world camera and game-type contexts.
    void reset() noexcept;

    // ---- local-player proxy (game thread) --------------------------------
    bool tick(void* clientInstance, std::uint64_t worldEpoch) noexcept;
    void* localActor() const noexcept;
    std::optional<CameraPlayerSnapshot> readPlayerProxy() const noexcept;
    CameraChange applyPlayerProxy(const CameraFrame& frame, std::uint64_t runtimeId,
                                  std::uint64_t epoch) noexcept;
    CameraChange restorePlayerProxy(const CameraPlayerSnapshot& saved) noexcept;

    // ---- camera rig (detached value state machine) ------------------------
    bool beginCamera(Vec3Value position, ActorBoundsValue bounds, std::uint64_t runtimeId,
                     std::uint64_t epoch) noexcept;
    std::optional<CameraFrame> advanceCamera(float yaw, const CameraMoveInput& input,
                                             const CameraMoveSpeed& speed, float elapsedSeconds,
                                             std::uint64_t runtimeId,
                                             std::uint64_t epoch) noexcept;
    void resetCamera() noexcept;
    bool cameraActive() const noexcept;
    CameraFrame frame() const noexcept;

    // ---- game type --------------------------------------------------------
    CameraGameType gameType() const noexcept;

    // ---- terrain culler / view factory / render observer ------------------
    bool installTerrain(bool rendererReady, std::string& error);
    bool installRenderObserve(bool rendererReady, std::string& error);
    bool installCreateView(std::string& error);
    // True when the world-render layout parsed and the outline renderer
    // loaded: player-hitbox capture and line drawing are available.
    bool renderReady() const noexcept;
    bool terrainReady() const noexcept;
    bool observeReady() const noexcept;
    bool createViewReady() const noexcept;
    bool leaseActive() const noexcept;
    void updateTerrain(void* renderer);
    void captureFromRenderer(void* renderer);
    std::uint64_t primaryCameraEpoch() const noexcept;
    void notePrimaryCamera(void* camera) noexcept;
    void noteNativeModeContext(std::uint64_t context) noexcept;
    void setDiagnosticGameType(int type, std::uint64_t epoch,
                               std::uint64_t milliseconds) noexcept;

    // ---- samples and statistics (value-returning, worker-safe) ------------
    std::optional<CameraVisibilitySample> takeSample() noexcept;
    CameraVisualStatistics takeStatistics() noexcept;
    CameraVisualStatistics statistics() const noexcept;

    // ---- inside-block-effect filter ---------------------------------------
    // Must be called before configure(): the native filter copies the policy
    // pointer when it installs. The default policy never suppresses.
    void setEffectPolicy(CameraEffectPolicy policy) noexcept;
    bool effectReady() const noexcept;
    // Exchanges the native counters, so each call covers the interval since
    // the previous one.
    CameraEffectStatistics takeEffectStatistics() noexcept;

    // ---- actor component access (game thread) -----------------------------
    bool hurtTime(void* actor, int& value) const noexcept;
    // Runtime entity id of any actor (the local-player proxy guard in
    // freecam re-reads it on the borrowed actor before restoring).
    std::optional<std::uint64_t> actorRuntimeId(void* actor) const noexcept;
    bool actorIdentity(void* actor, std::uintptr_t& registry, std::uint32_t& entity) const noexcept;
    bool restoreActorMotion(void* actor, Vec3Value position, ActorBoundsValue bounds) noexcept;
    // `out.origin` must hold the render origin to subtract and `out.epoch` the
    // world epoch; both are read before capture and echoed back.
    bool capturePlayerBounds(void* localActor, std::uint64_t epoch, unsigned rows,
                             PlayerBoundsFrame& out) noexcept;

    // ---- packet sender (game thread) --------------------------------------
    bool resolveSender(void* client, void*& sender, void*& target) const noexcept;

    // ---- render callback + outline drawing (render thread) ----------------
    // Subscribe the camera-visual render pass. The handler runs after the
    // original render call with borrowed renderer/screen handles.
    bool installRenderHook(CameraRenderHandler handler, void* user, std::string& error);
    void stopRenderHook() noexcept;
    void* renderTarget() const noexcept;
    // Draw outcome, mirroring the native renderer's tri-state: Skipped is
    // a benign per-frame condition (renderer busy/not ready), Fault is a
    // real failure. Callers must not treat Skipped as an error.
    CameraDrawResult drawLines(void* renderer, void* screen, Vec3Value origin,
                               const CameraLineSegment* segments, std::size_t count,
                               std::array<float, 4> rgba) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bedrock::sdk
