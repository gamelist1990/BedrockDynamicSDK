#pragma once
#include <bedrock/game_api.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <vector>
#include <utility>

namespace hitbox {
using Clock = std::chrono::steady_clock;
struct Sample {
    std::uint64_t runtimeId{};
    bedrock::ActorBounds bounds{};
    Clock::time_point seenAt{};
    bool isPlayer{};
    std::uintptr_t actorKey{};
    std::uint32_t entityId{};
    std::uint64_t uniqueId{};
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
// A one-tick target sweep is spread across callbacks; slow callbacks keep the cursor
// and finish the pending sweep rather than restarting and starving later rows.
class CaptureSchedule {
public:
    static constexpr std::uint64_t periodTicks=1;
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
// Shared admission only; native capture/render threads never take this mutex.
// Both live and saved copies of a RuntimeID consume one slot across all workers.
class TrackingBudget {
public:
    static constexpr std::size_t maximumActors=1000;
    bool beginWorld(std::uint64_t epoch) {
        std::lock_guard lock(mutex_);
        if(epoch<epoch_) return false;
        if(epoch!=epoch_) { ids_.clear(); epoch_=epoch; }
        return true;
    }
    bool acquire(std::uint64_t id,std::uint64_t epoch) {
        std::lock_guard lock(mutex_);
        if(epoch!=epoch_) return false;
        if(ids_.contains(id)) return true;
        if(ids_.size()>=maximumActors) return false;
        ids_.insert(id); return true;
    }
    void release(std::uint64_t id,std::uint64_t epoch) {
        std::lock_guard lock(mutex_);
        if(epoch==epoch_) ids_.erase(id);
    }
    std::size_t size() const {
        std::lock_guard lock(mutex_); return ids_.size();
    }
private:
    mutable std::mutex mutex_;
    std::uint64_t epoch_{};
    std::unordered_set<std::uint64_t> ids_;
};
// Game-thread-owned identity history. Only confirmed RuntimeIDs are remembered;
// an unrelated replacement entity is never inferred to be the same Player.
class PlayerIdentityCache {
public:
    static constexpr std::size_t maximumPlayers=TrackingBudget::maximumActors;
    void reset() noexcept { players_.fill(0); size_=0; }
    bool known(std::uint64_t runtimeId) const noexcept {
        return runtimeId && players_[slot(runtimeId)]==runtimeId;
    }
    bool classify(std::uint64_t runtimeId, bool nativePlayer) noexcept {
        if (!runtimeId) return false;
        if (nativePlayer) {
            const auto index=slot(runtimeId);
            if (!players_[index] && size_<maximumPlayers) {
                players_[index]=runtimeId; ++size_;
            }
            return true;
        }
        return known(runtimeId);
    }
private:
    // Fixed storage, under 50% occupancy: no allocations or locks in native capture.
    static constexpr std::size_t tableSize=2048;
    static_assert(maximumPlayers<tableSize/2);
    std::size_t slot(std::uint64_t id) const noexcept {
        auto hash=id;
        hash=(hash^(hash>>30))*0xbf58476d1ce4e5b9ULL;
        hash=(hash^(hash>>27))*0x94d049bb133111ebULL;
        auto index=static_cast<std::size_t>(hash^(hash>>31))&(tableSize-1);
        while(players_[index] && players_[index]!=id) index=(index+1)&(tableSize-1);
        return index;
    }
    std::array<std::uint64_t,tableSize> players_{};
    std::size_t size_{};
};
// Worker-owned state. No native handles, component pointers or game calls.
class BoxCache {
public:
    static constexpr std::size_t maximumRecords = TrackingBudget::maximumActors;
    explicit BoxCache(std::shared_ptr<TrackingBudget> budget=std::make_shared<TrackingBudget>())
        : budget_(std::move(budget)) {}
    BoxCache(const BoxCache&)=delete;
    BoxCache& operator=(const BoxCache&)=delete;
    bool beginWorld(std::uint64_t epoch) {
        if (epoch < epoch_ || !budget_->beginWorld(epoch)) return false;
        if (epoch != epoch_) { records_.clear(); retained_.clear(); retaining_=false; epoch_=epoch; }
        return true;
    }
    void observe(const Sample& sample, const bedrock::Vec3& camera, float range, bool retain, bool allActors=false) {
        configureRetention(retain);
        const auto& bounds=sample.bounds;
        if (!sample.runtimeId || !bounds.valid() || bounds.minimum.x==bounds.maximum.x ||
            bounds.minimum.y==bounds.maximum.y || bounds.minimum.z==bounds.maximum.z) return;
        const auto old=records_.find(sample.runtimeId);
        const auto saved=retained_.find(sample.runtimeId);
        if ((old!=records_.end() && old->second.box.seenAt>=sample.seenAt) ||
            (saved!=retained_.end() && saved->second.seenAt>=sample.seenAt)) return;
        if (!allActors && distanceSquared(center(sample.bounds),camera)>range*range) {
            records_.erase(sample.runtimeId);
            retained_.erase(sample.runtimeId); // A fresh out-of-range observation is not disappearance.
            budget_->release(sample.runtimeId,epoch_);
            return;
        }
        if(old==records_.end() && !budget_->acquire(sample.runtimeId,epoch_)) return;
        Entry entry;
        if(old!=records_.end()) entry=old->second;
        const auto previousSeen=old!=records_.end() ? old->second.box.seenAt : Clock::time_point{};
        auto& box=entry.box;
        static_cast<Sample&>(box)=sample;
        box.corners=hitbox::corners(sample.bounds);
        box.blendFrom=sample.bounds;
        box.blendDuration=std::chrono::milliseconds(0);
        box.retained=false;
        // Preserve classification if this same RuntimeID changes ActorType.
        box.isPlayer=sample.isPlayer || (old!=records_.end() && old->second.box.isPlayer) ||
                     retained_.contains(sample.runtimeId);
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
        }
        // Qualification requires actual observed motion before a full second
        // of observed stillness. Missing reads never advance this timer.
        const auto position=feet(box.bounds);
        if(!retain || old==records_.end() || sample.seenAt-previousSeen>std::chrono::seconds(2)) {
            entry.moved=false; entry.stationaryConfirmed=false;
            entry.stationaryAnchor=position; entry.stationarySince=sample.seenAt;
        } else if((!entry.moved && distanceSquared(position,entry.stationaryAnchor)>0.03f*0.03f) ||
                  distanceSquared(position,entry.stationaryAnchor)>1.0f) {
            entry.moved=true; entry.stationaryConfirmed=false;
            entry.stationaryAnchor=position; entry.stationarySince=sample.seenAt;
        } else {
            entry.stationaryConfirmed=entry.moved && sample.seenAt-entry.stationarySince>=std::chrono::seconds(1);
        }
        // Returning/moving Actors must not leave an older saved box behind.
        retained_.erase(sample.runtimeId);
        records_.insert_or_assign(sample.runtimeId,std::move(entry));
    }
    std::vector<BoxRecord> visible(Clock::time_point now, const bedrock::Vec3& camera,
        float range, bool retain, std::chrono::milliseconds liveLifetime, bool allActors=false, bool playersOnly=false,
        std::chrono::milliseconds retainedLifetime=std::chrono::minutes(10)) {
        configureRetention(retain);
        for (auto it=retained_.begin();it!=retained_.end();) {
            if (now-it->second.seenAt>=retainedLifetime) {
                const auto live=records_.find(it->first);
                if(live==records_.end()) budget_->release(it->first,epoch_);
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
            if (now-it->second.box.seenAt>liveLifetime) {
                const auto& entry=it->second;
                if(retain && entry.box.isPlayer && entry.stationaryConfirmed &&
                    now-entry.box.seenAt<retainedLifetime) {
                    auto frozen=entry.box;
                    frozen.retained=true; frozen.blendFrom=frozen.bounds;
                    frozen.blendDuration=std::chrono::milliseconds(0);
                    retained_.insert_or_assign(it->first,std::move(frozen));
                }
                if(!retained_.contains(it->first)) budget_->release(it->first,epoch_);
                it=records_.erase(it);
            }
            else {
                if (permitted(it->second.box)) result.push_back(it->second.box);
                ++it;
            }
        }
        if (retain) for (const auto& [id,frozen]:retained_) {
            const auto live=records_.find(id);
            // While tracked, draw the current position only. The last valid
            // position is a fallback, not an extra historical trail beside a moving Player.
            if (live!=records_.end() && permitted(live->second.box)) continue;
            if (permitted(frozen)) result.push_back(frozen);
        }
        return result;
    }
    std::size_t size() const noexcept { return records_.size(); }
    std::size_t retainedSize() const noexcept { return retained_.size(); }
private:
    static bedrock::Vec3 feet(const bedrock::ActorBounds& bounds) noexcept {
        return {(bounds.minimum.x+bounds.maximum.x)*0.5f,bounds.minimum.y,
                (bounds.minimum.z+bounds.maximum.z)*0.5f};
    }
    struct Entry {
        BoxRecord box;
        bedrock::Vec3 stationaryAnchor{};
        Clock::time_point stationarySince{};
        bool moved{},stationaryConfirmed{};
    };
    void configureRetention(bool retain) {
        if (retaining_==retain) return;
        retaining_=retain;
        for(const auto& [id,box]:retained_)
            if(!records_.contains(id)) budget_->release(id,epoch_);
        retained_.clear();
        for(auto& [id,entry]:records_) {
            entry.moved=entry.stationaryConfirmed=false;
            entry.stationaryAnchor=feet(entry.box.bounds);
            entry.stationarySince=entry.box.seenAt;
        }
    }
    std::shared_ptr<TrackingBudget> budget_;
    bool retaining_{};
    std::uint64_t epoch_{};
    std::unordered_map<std::uint64_t,Entry> records_;
    std::unordered_map<std::uint64_t,BoxRecord> retained_;
};
} // namespace hitbox
