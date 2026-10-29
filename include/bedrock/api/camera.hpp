#pragma once
#include <bedrock/api/geometry.hpp>
#include <optional>
#include <bedrock/api/api_types.hpp>
#include <chrono>
#include <algorithm>
#include <cmath>
namespace bedrock {
// Pure value operations: safe on copied worker snapshots, no game access.
struct CameraInput { float forward{}, right{}, up{}; };
struct CameraSpeed { float horizontal=1.f, vertical=.5f; };
inline Vec3 moveCamera(Vec3 position,float yaw,CameraInput input,CameraSpeed speed){
    if(!std::isfinite(yaw)||!std::isfinite(input.forward)||!std::isfinite(input.right)||!std::isfinite(input.up)||
       !std::isfinite(speed.horizontal)||!std::isfinite(speed.vertical))return position;
    const float r=yaw*.0174532925199433f;
    const float length=std::max(1.f,std::sqrt(input.forward*input.forward+input.right*input.right));
    position.x+=(-std::sin(r)*input.forward-std::cos(r)*input.right)*speed.horizontal/length;
    position.z+=(std::cos(r)*input.forward-std::sin(r)*input.right)*speed.horizontal/length;
    position.y+=input.up*speed.vertical;
    return position;
}
inline Vec3 interpolateCamera(Vec3 previous,Vec3 current,float alpha){
    if(!std::isfinite(alpha))alpha=0;
    alpha=std::clamp(alpha,0.f,1.f);
    return {previous.x+(current.x-previous.x)*alpha,previous.y+(current.y-previous.y)*alpha,previous.z+(current.z-previous.z)*alpha};
}
inline ActorBounds translateBounds(ActorBounds box,Vec3 from,Vec3 to){
    const Vec3 d{to.x-from.x,to.y-from.y,to.z-from.z};
    box.minimum={box.minimum.x+d.x,box.minimum.y+d.y,box.minimum.z+d.z};
    box.maximum={box.maximum.x+d.x,box.maximum.y+d.y,box.maximum.z+d.z};return box;
}
} // namespace bedrock

namespace bedrock {
struct CameraFrame { Vec3 position{}, previous{}; ActorBounds bounds{}; };
// Detached camera state machine. Packet anchoring and rendering are supplied by
// the host integration. Never changes GameType or sends packets by itself.
class CameraRig {
public:
    bool begin(Vec3 position,ActorBounds bounds,std::uint64_t runtimeId,std::uint64_t worldEpoch){
        reset();if(!finite(position)||!bounds.valid()||bounds.minimum.x>=bounds.maximum.x||bounds.minimum.y>=bounds.maximum.y||bounds.minimum.z>=bounds.maximum.z||!runtimeId)return false;
        anchor_=position;anchorBounds_=bounds;frame_={position,position,bounds};runtime_=runtimeId;epoch_=worldEpoch;active_=true;return true;
    }
    std::optional<CameraFrame> update(float yaw,CameraInput input,CameraSpeed speed,
                                      std::uint64_t runtimeId,std::uint64_t worldEpoch,
                                      bool damaged=false,bool disableOnDamage=true){
        if(!active_||runtimeId!=runtime_||worldEpoch!=epoch_||(damaged&&disableOnDamage)){reset();return {};}
        const auto next=moveCamera(frame_.position,yaw,input,speed);
        if(!finite(next)){reset();return {};}
        frame_.previous=frame_.position;frame_.position=next;frame_.bounds=translateBounds(anchorBounds_,anchor_,next);
        return frame_;
    }
    // Speeds are blocks per second; elapsed time is explicit. The legacy
    // update() keeps its per-call units for source compatibility.
    std::optional<CameraFrame> advance(Yaw yaw, CameraInput input, CameraSpeed speed,
                                      std::chrono::duration<float> elapsed,
                                      std::uint64_t runtimeId, std::uint64_t worldEpoch) {
        if (!active_ || runtimeId != runtime_ || worldEpoch != epoch_) { reset(); return std::nullopt; }
        const float seconds = elapsed.count();
        if (!std::isfinite(input.forward) || !std::isfinite(input.right) || !std::isfinite(input.up) ||
            std::abs(input.forward) > 1.f || std::abs(input.right) > 1.f || std::abs(input.up) > 1.f ||
            !std::isfinite(seconds) || seconds < 0.f || seconds > .25f ||
            !std::isfinite(speed.horizontal) || !std::isfinite(speed.vertical) ||
            speed.horizontal < 0.f || speed.vertical < 0.f) return std::nullopt;
        const auto result = update(yaw.degrees(), input, {speed.horizontal * seconds, speed.vertical * seconds}, runtimeId, worldEpoch);
        if (result) rotation_.yaw = yaw.degrees();
        return result;
    }
    bool look(Yaw yaw, Pitch pitch) noexcept {
        if (!active_) return false;
        rotation_ = {pitch.degrees(), yaw.degrees()}; return true;
    }
    bool moveTo(Vec3 position) noexcept {
        if (!active_ || !finite(position)) return false;
        frame_.previous = frame_.position; frame_.position = position;
        frame_.bounds = translateBounds(anchorBounds_, anchor_, position); return true;
    }
    std::optional<CameraFrame> frame() const noexcept { return active_ ? std::optional{frame_} : std::nullopt; }
    std::optional<Vec3> sample(float alpha) const noexcept {
        return active_ ? std::optional{interpolateCamera(frame_.previous, frame_.position, alpha)} : std::nullopt;
    }
    ActorRotation rotation() const noexcept { return rotation_; }
    bool active()const noexcept{return active_;}
    Vec3 anchor()const noexcept{return anchor_;}
    void reset()noexcept{active_=false;runtime_=epoch_=0;frame_={};anchor_={};anchorBounds_={};rotation_={};}
private:
    static bool finite(Vec3 p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::abs(p.x)<30000000&&std::abs(p.z)<30000000&&std::abs(p.y)<10000;}
    bool active_{};std::uint64_t runtime_{},epoch_{};Vec3 anchor_{};ActorBounds anchorBounds_{};CameraFrame frame_{};ActorRotation rotation_{};
};
using FreeCameraRig = CameraRig; // compatibility name; new integrations use CameraRig
} // namespace bedrock
