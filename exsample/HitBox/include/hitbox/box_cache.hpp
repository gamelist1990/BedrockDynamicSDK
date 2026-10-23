#pragma once
#include <bedrock/game_api.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <utility>

namespace hitbox {
using Clock = std::chrono::steady_clock;
struct Sample {
    std::uint64_t runtimeId{};
    bedrock::ActorBounds bounds{};
    Clock::time_point seenAt{};
};
struct BoxRecord : Sample {
    std::array<bedrock::Vec3, 8> corners{};
    bedrock::ActorBounds blendFrom{};
    std::chrono::milliseconds blendDuration{};
};
inline float distanceSquared(const bedrock::Vec3& a, const bedrock::Vec3& b) noexcept {
    const float x=a.x-b.x, y=a.y-b.y, z=a.z-b.z;
    return x*x+y*y+z*z;
}
inline bedrock::Vec3 center(const bedrock::ActorBounds& b) noexcept {
    return {(b.minimum.x+b.maximum.x)*0.5f, (b.minimum.y+b.maximum.y)*0.5f,
            (b.minimum.z+b.maximum.z)*0.5f};
}
inline std::array<bedrock::Vec3, 8> corners(const bedrock::ActorBounds& b) noexcept {
    return {{{b.minimum.x,b.minimum.y,b.minimum.z}, {b.maximum.x,b.minimum.y,b.minimum.z},
        {b.maximum.x,b.minimum.y,b.maximum.z}, {b.minimum.x,b.minimum.y,b.maximum.z},
        {b.minimum.x,b.maximum.y,b.minimum.z}, {b.maximum.x,b.maximum.y,b.minimum.z},
        {b.maximum.x,b.maximum.y,b.maximum.z}, {b.minimum.x,b.maximum.y,b.maximum.z}}};
}
// A ten-tick sweep is spread across callbacks; slow callbacks keep the cursor
// and finish the pending sweep rather than restarting and starving later rows.
class CaptureSchedule {
public:
    static constexpr std::uint64_t periodTicks=10;
    static constexpr std::size_t maximumBatch=32;
    void reset() noexcept { tick_=nextSweep_=0; remaining_=population_=0; }
    std::size_t beginTick(std::size_t population) noexcept {
        const auto tick=tick_++;
        if (!population) { remaining_=population_=0; nextSweep_=tick; return 0; }
        if (!remaining_) {
            if (tick<nextSweep_) return 0;
            population_=population; remaining_=population; nextSweep_=tick+periodTicks;
        }
        remaining_=std::min(remaining_,population);
        return std::min({maximumBatch,remaining_,std::max<std::size_t>(1,(population_+periodTicks-1)/periodTicks)});
    }
    void visited(std::size_t count) noexcept { remaining_-=std::min(count,remaining_); }
private:
    std::uint64_t tick_{},nextSweep_{};
    std::size_t remaining_{},population_{};
};
inline bedrock::ActorBounds interpolatedBounds(const BoxRecord& box, Clock::time_point now) noexcept {
    if (box.blendDuration.count()<=0) return box.bounds;
    const float alpha=std::clamp(std::chrono::duration<float>(now-box.seenAt).count()/
        std::chrono::duration<float>(box.blendDuration).count(),0.f,1.f);
    const auto lerp=[alpha](const bedrock::Vec3& from,const bedrock::Vec3& to) {
        return bedrock::Vec3{from.x+(to.x-from.x)*alpha,from.y+(to.y-from.y)*alpha,from.z+(to.z-from.z)*alpha};
    };
    return {lerp(box.blendFrom.minimum,box.bounds.minimum),lerp(box.blendFrom.maximum,box.bounds.maximum)};
}
// The engine's 3D pass already owns the view/projection matrices. Supply line
// vertices relative to its render origin; do not add an eye-height or FOV guess.
inline std::array<bedrock::Vec3, 24> worldLineVertices(
    const BoxRecord& box, const bedrock::Vec3& renderOrigin,
    Clock::time_point now=Clock::time_point::max()) noexcept {
    constexpr std::array<std::array<unsigned, 2>, 12> edges{{
        {{0,1}},{{1,2}},{{2,3}},{{3,0}},{{4,5}},{{5,6}},{{6,7}},{{7,4}},
        {{0,4}},{{1,5}},{{2,6}},{{3,7}}}};
    const auto currentCorners=now==Clock::time_point::max() ? box.corners : corners(interpolatedBounds(box,now));
    std::array<bedrock::Vec3, 24> result{};
    std::size_t index = 0;
    for (const auto& edge : edges) {
        for (const auto corner : edge) {
            const auto& point = currentCorners[corner];
            result[index++] = {point.x - renderOrigin.x, point.y - renderOrigin.y,
                               point.z - renderOrigin.z};
        }
    }
    return result;
}
// Worker-owned state. No native handles, component pointers or game calls.
class BoxCache {
public:
    static constexpr std::size_t maximumRecords = 4096;
    bool beginWorld(std::uint64_t epoch) {
        if (epoch < epoch_) return false;
        if (epoch != epoch_) { records_.clear(); epoch_=epoch; }
        return true;
    }
    void observe(const Sample& sample, const bedrock::Vec3& camera, float range, bool retain) {
        if (!sample.runtimeId || !sample.bounds.valid()) return;
        if (distanceSquared(center(sample.bounds),camera)>range*range) {
            if (!retain) records_.erase(sample.runtimeId);
            return;
        }
        const auto old=records_.find(sample.runtimeId);
        if (old!=records_.end() && old->second.seenAt>sample.seenAt) return;
        if (old==records_.end() && records_.size()>=maximumRecords) {
            const auto oldest=std::min_element(records_.begin(),records_.end(),
                [](const auto& a,const auto& b){return a.second.seenAt<b.second.seenAt;});
            records_.erase(oldest);
        }
        BoxRecord box;
        static_cast<Sample&>(box)=sample;
        box.corners=hitbox::corners(sample.bounds);
        box.blendFrom=sample.bounds;
        if (old!=records_.end()) {
            const auto age=sample.seenAt-old->second.seenAt;
            const auto from=interpolatedBounds(old->second,sample.seenAt);
            // Snap teleports and stale/reused IDs; never extrapolate unknown movement.
            if (age>Clock::duration::zero() && age<=std::chrono::seconds(2) &&
                distanceSquared(center(from),center(sample.bounds))<=64.f) {
                box.blendFrom=from;
                box.blendDuration=std::clamp(std::chrono::duration_cast<std::chrono::milliseconds>(age),
                    std::chrono::milliseconds(50),std::chrono::milliseconds(1000));
            }
        }
        records_.insert_or_assign(sample.runtimeId,std::move(box));
    }
    std::vector<BoxRecord> visible(Clock::time_point now, const bedrock::Vec3& camera,
        float range, bool retain, unsigned minutes, std::chrono::milliseconds liveLifetime) {
        std::vector<BoxRecord> result;
        result.reserve(records_.size());
        for (auto it=records_.begin();it!=records_.end();) {
            if (now-it->second.seenAt>std::chrono::minutes(minutes) ||
                (!retain && now-it->second.seenAt>liveLifetime)) {
                it=records_.erase(it);
            } else {
                if (distanceSquared(center(it->second.bounds),camera)<=range*range)
                    result.push_back(it->second);
                ++it;
            }
        }
        return result;
    }
    std::size_t size() const noexcept { return records_.size(); }
private:
    std::uint64_t epoch_{};
    std::unordered_map<std::uint64_t, BoxRecord> records_;
};
} // namespace hitbox
