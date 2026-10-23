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
    bool isPlayer{};
};
struct BoxRecord : Sample {
    std::array<bedrock::Vec3, 8> corners{};
    bedrock::ActorBounds blendFrom{};
    std::chrono::milliseconds blendDuration{};
    bool retained{};
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
// A five-tick sweep is spread across callbacks; slow callbacks keep the cursor
// and finish the pending sweep rather than restarting and starving later rows.
class CaptureSchedule {
public:
    static constexpr std::uint64_t periodTicks=5;
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
// Position-only/current_color materials read the shader uniform, not vertex
// RGBA. Force opacity even when an older config still contains translucent alpha.
inline std::array<float,4> opaqueColor(std::uint32_t packed) noexcept {
    return {{(packed&255)/255.f,((packed>>8)&255)/255.f,((packed>>16)&255)/255.f,1.f}};
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
inline bool visibleInFrame(const BoxRecord& box, Clock::time_point now,
    std::chrono::milliseconds lifetime, bool retain, bool playersOnly,
    std::chrono::milliseconds retainedLifetime=std::chrono::minutes(10)) noexcept {
    if (playersOnly && !box.isPlayer) return false;
    if (box.retained) return retain && box.isPlayer && now-box.seenAt<retainedLifetime;
    return now-box.seenAt<=lifetime;
}
// Worker-owned state. No native handles, component pointers or game calls.
class BoxCache {
public:
    static constexpr std::size_t maximumRecords = 16384; // Live records only.
    static constexpr auto stationaryDelay = std::chrono::seconds(2);
    static constexpr auto maximumObservationGap = std::chrono::seconds(1);
    static constexpr float stationaryToleranceSquared = 0.05f*0.05f;
    bool beginWorld(std::uint64_t epoch) {
        if (epoch < epoch_) return false;
        if (epoch != epoch_) { records_.clear(); retained_.clear(); retaining_=false; epoch_=epoch; }
        return true;
    }
    static bool samePosition(const bedrock::ActorBounds& a,const bedrock::ActorBounds& b) noexcept {
        return distanceSquared(a.minimum,b.minimum)<=stationaryToleranceSquared &&
               distanceSquared(a.maximum,b.maximum)<=stationaryToleranceSquared;
    }
    void observe(const Sample& sample, const bedrock::Vec3& camera, float range, bool retain, bool allActors=false) {
        configureRetention(retain);
        if (!sample.runtimeId || !sample.bounds.valid()) return;
        if (!allActors && distanceSquared(center(sample.bounds),camera)>range*range) {
            records_.erase(sample.runtimeId); // Out-of-range reads cannot confirm a stop.
            return;
        }
        const auto old=records_.find(sample.runtimeId);
        if (old!=records_.end() && old->second.box.seenAt>=sample.seenAt) return;
        if (old==records_.end() && records_.size()>=maximumRecords) {
            const auto oldest=std::min_element(records_.begin(),records_.end(),
                [](const auto& a,const auto& b){return a.second.box.seenAt<b.second.box.seenAt;});
            records_.erase(oldest);
        }
        Entry entry;
        auto& box=entry.box;
        static_cast<Sample&>(box)=sample;
        box.corners=hitbox::corners(sample.bounds);
        box.blendFrom=sample.bounds;
        entry.stationaryAnchor=sample.bounds;
        entry.stationarySince=sample.seenAt;
        if (old!=records_.end()) {
            const auto& previous=old->second;
            const auto age=sample.seenAt-previous.box.seenAt;
            const auto from=interpolatedBounds(previous.box,sample.seenAt);
            if (age<=std::chrono::seconds(2) &&
                distanceSquared(center(from),center(sample.bounds))<=64.f) {
                box.blendFrom=from;
                box.blendDuration=std::clamp(std::chrono::duration_cast<std::chrono::milliseconds>(age),
                    std::chrono::milliseconds(50),std::chrono::milliseconds(1000));
            }
            // Confirm only from repeated real observations, never from an old
            // snapshot freezing after a failed read, unload or missed capture.
            if (retain && sample.isPlayer && previous.box.isPlayer && previous.tracking &&
                age<=maximumObservationGap && samePosition(sample.bounds,previous.stationaryAnchor)) {
                entry.stationaryAnchor=previous.stationaryAnchor;
                entry.stationarySince=previous.stationarySince;
                entry.committed=previous.committed;
            }
        }
        entry.tracking=retain && sample.isPlayer;
        if (entry.tracking && !entry.committed && sample.seenAt-entry.stationarySince>=stationaryDelay) {
            auto frozen=box;
            frozen.retained=true;
            frozen.blendFrom=frozen.bounds;
            frozen.blendDuration=std::chrono::milliseconds(0);
            // One last confirmed stop per RuntimeID; each new stop restarts its expiry.
            retained_.insert_or_assign(sample.runtimeId,std::move(frozen));
            entry.committed=true;
        }
        records_.insert_or_assign(sample.runtimeId,std::move(entry));
    }
    std::vector<BoxRecord> visible(Clock::time_point now, const bedrock::Vec3& camera,
        float range, bool retain, std::chrono::milliseconds liveLifetime, bool allActors=false, bool playersOnly=false,
        std::chrono::milliseconds retainedLifetime=std::chrono::minutes(10)) {
        configureRetention(retain);
        for (auto it=retained_.begin();it!=retained_.end();) {
            if (now-it->second.seenAt>=retainedLifetime) {
                const auto live=records_.find(it->first);
                if (live!=records_.end()) {
                    // Require another two seconds of actual observations before
                    // saving a still-present Player again after expiry.
                    live->second.committed=false;
                    live->second.stationarySince=now;
                    live->second.stationaryAnchor=live->second.box.bounds;
                }
                it=retained_.erase(it);
            } else ++it;
        }
        std::vector<BoxRecord> result;
        result.reserve(records_.size()+retained_.size());
        const auto permitted=[&](const BoxRecord& box) {
            return (!playersOnly || box.isPlayer) &&
                   (allActors || distanceSquared(center(box.bounds),camera)<=range*range);
        };
        for (auto it=records_.begin();it!=records_.end();) {
            if (now-it->second.box.seenAt>liveLifetime) it=records_.erase(it);
            else {
                if (permitted(it->second.box)) result.push_back(it->second.box);
                ++it;
            }
        }
        if (retain) for (const auto& [id,frozen]:retained_) {
            const auto live=records_.find(id);
            // While tracked, draw the current position only. The last confirmed
            // stop is a fallback, not an extra historical trail beside a moving Player.
            if (live!=records_.end() && permitted(live->second.box)) continue;
            if (permitted(frozen)) result.push_back(frozen);
        }
        return result;
    }
    std::size_t size() const noexcept { return records_.size(); }
    std::size_t retainedSize() const noexcept { return retained_.size(); }
private:
    struct Entry {
        BoxRecord box;
        bedrock::ActorBounds stationaryAnchor{};
        Clock::time_point stationarySince{};
        bool tracking{},committed{};
    };
    void configureRetention(bool retain) {
        if (retaining_==retain) return;
        retaining_=retain;
        retained_.clear();
        for (auto& [id,entry]:records_) { entry.tracking=false; entry.committed=false; }
    }
    bool retaining_{};
    std::uint64_t epoch_{};
    std::unordered_map<std::uint64_t,Entry> records_;
    std::unordered_map<std::uint64_t,BoxRecord> retained_;
};
} // namespace hitbox
