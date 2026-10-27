#include <bedrock/camera.hpp>
#include <bedrock/brightness.hpp>
#include <bedrock/game_type.hpp>

std::optional<bedrock::CameraFrame> moveCopiedCamera(bedrock::CameraRig& camera,
    bedrock::Yaw yaw, bedrock::CameraInput input, std::uint64_t runtime, std::uint64_t epoch) {
    // Speeds: horizontal/vertical blocks per second. Elapsed time is explicit.
    return camera.advance(yaw, input, {10.f, 5.f}, std::chrono::duration<float>{.05f}, runtime, epoch);
}

#ifdef _WIN32
#include <bedrock/native_camera.hpp>
#include <bedrock/native_game_type.hpp>
#include <bedrock/native_brightness.hpp>

// Prepare these objects from RuntimeProfile first; install the brightness hook
// after every adapter has passed prepare(). Keep them alive for the Mod lifetime.
void useNativeCameraOnLevelTick(bedrock::NativeCamera& camera, bedrock::NativeGameType& types,
    void* client, bedrock::WorldEpoch epoch, const bedrock::CameraFrame& frame,
    const bedrock::CameraPlayerSnapshot& saved) {
    auto cameraScope = camera.tick(client, epoch);
    auto typeScope = types.tick(client, epoch);
    if (!cameraScope || !typeScope) return;
    const auto current = camera.readPlayerProxy();
    if (!current) return;
    const auto moved = camera.applyPlayerProxy(frame, current->runtimeId, epoch);
    const auto restored = camera.restorePlayerProxy(saved); // reject stale runtime/world
    const auto rotation = camera.setRotation(bedrock::Yaw{90.f}, bedrock::Pitch{0.f});
    const auto gameType = types.gameType();
    // This explicitly changes local game mode, and is separate from camera
    // rendering. Remote authority and server acceptance are not implied.
    const auto changed = types.setGameType(bedrock::GameType::Creative);
    (void)moved; (void)restored; (void)rotation; (void)gameType; (void)changed;
}
#endif
