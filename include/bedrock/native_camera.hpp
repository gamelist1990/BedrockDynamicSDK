#pragma once
#include <bedrock/camera.hpp>
#include <bedrock/native_motion.hpp>
#include <bedrock/detail/native_player_context.hpp>

namespace bedrock {
struct CameraPlayerSnapshot {
    std::uint64_t runtimeId{}, worldEpoch{};
    Vec3 position{}, previous{}, velocity{};
    float fallDistance{};
    ActorBounds bounds{};
    ActorRotation rotation{};
};
enum class CameraChange { Applied, NoPlayer, WrongThread, WorldChanged, InvalidValue, NativeFailure };

// Owner-tick local-player proxy operations used with CameraRig. Network packet
// anchoring and controls remain the host Mod's policy; no Freecam API is needed.
class NativeCamera {
public:
    using Scope = native_player::Context::Scope;
    bool load(const RuntimeProfile& profile, std::string& error) {
        ready_ = false;
        if (!profile.supports({Feature::Camera, Feature::Motion}, error)) return false;
        try {
            motion_ = native_motion::Layout::fromProfile(profile.json());
            ready_ = context_.load(profile, error); return ready_;
        } catch (const std::exception& e) { error = e.what(); return false; }
    }
    void reset() noexcept { context_.reset(); }
    Scope tick(void* client, WorldEpoch epoch) { return context_.tick(ready_ ? client : nullptr, epoch); }
    // Borrowed only while the caller retains this adapter's owning tick Scope.
    void* localActor() const { return context_.valid() ? context_.actor() : nullptr; }
    std::optional<CameraPlayerSnapshot> readPlayerProxy() const {
        if (!context_.valid()) return std::nullopt;
        native_motion::Snapshot state;
        const auto get = [this](void*, std::uint32_t hash) { return context_.component(hash); };
        if (!native_motion::capture(context_.actor(), context_.layout(), motion_, get, state)) return std::nullopt;
        ActorRotation rotation;
        const auto* value = context_.component(context_.layout().actorRotationComponentHash);
        if (!world_native::read(value, context_.layout().rotationPitchOffset, rotation.pitch) ||
            !world_native::read(value, context_.layout().rotationYawOffset, rotation.yaw) ||
            !std::isfinite(rotation.pitch) || !std::isfinite(rotation.yaw)) return std::nullopt;
        return CameraPlayerSnapshot{context_.runtimeId(), context_.epoch(), state.position, state.previous, state.velocity, state.fallDistance, state.bounds, rotation};
    }
    CameraChange applyPlayerProxy(const CameraFrame& frame, std::uint64_t runtimeId, WorldEpoch epoch) {
        if (!context_.onThread()) return CameraChange::WrongThread;
        if (!context_.valid() || context_.runtimeId() != runtimeId || epoch.value() != context_.epoch()) return CameraChange::WorldChanged;
        if (!native_motion::finite(frame.position) || !native_motion::finite(frame.previous) || !frame.bounds.valid())
            return CameraChange::InvalidValue;
        const auto get = [this](void*, std::uint32_t hash) { return context_.component(hash); };
        native_motion::Snapshot current;
        if (!native_motion::capture(context_.actor(), context_.layout(), motion_, get, current)) return CameraChange::NativeFailure;
        // Derive AABB from the live shape; never trust a caller-supplied body size.
        const auto bounds = translateBounds(current.bounds, current.position, frame.position);
        const native_motion::Snapshot next{frame.position, frame.previous, Vec3{}, bounds, 0.f};
        return native_motion::apply(context_.actor(), context_.layout(), motion_, get, next) ? CameraChange::Applied : CameraChange::NativeFailure;
    }
    CameraChange restorePlayerProxy(const CameraPlayerSnapshot& saved) {
        return applyPlayerProxy({saved.position, saved.position, saved.bounds}, saved.runtimeId, WorldEpoch{saved.worldEpoch});
    }
    CameraChange setRotation(Yaw yaw, Pitch pitch) {
        if (!context_.onThread()) return CameraChange::WrongThread;
        if (!context_.valid()) return CameraChange::NoPlayer;
        auto* rotation = context_.component(context_.layout().actorRotationComponentHash);
        float oldYaw{}, oldPitch{};
        if (!world_native::read(rotation, context_.layout().rotationYawOffset, oldYaw) ||
            !world_native::read(rotation, context_.layout().rotationPitchOffset, oldPitch)) return CameraChange::NativeFailure;
        bool ok = native_motion::write(rotation, context_.layout().rotationYawOffset, yaw.degrees());
        ok = native_motion::write(rotation, context_.layout().rotationPitchOffset, pitch.degrees()) && ok;
        if (ok) return CameraChange::Applied;
        native_motion::write(rotation, context_.layout().rotationYawOffset, oldYaw);
        native_motion::write(rotation, context_.layout().rotationPitchOffset, oldPitch);
        return CameraChange::NativeFailure;
    }
private:
    native_player::Context context_;
    native_motion::Layout motion_;
    bool ready_{};
};
} // namespace bedrock
