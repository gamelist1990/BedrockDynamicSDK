#pragma once
#include "box_cache.hpp"

namespace hitbox {
// Fast path for live geometry. Only already-copied game-thread scalars cross
// this mailbox. Font preparation/worker publication cannot delay a new pose.
class LatestPoseMailbox {
public:
    void publish(std::uint64_t epoch,const Sample* samples,std::size_t count) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock) return;
        for(std::size_t i=0;i<count;++i) {
            const auto& sample=samples[i];if(!sample.runtimeId)continue;
            auto first=bucket(sample.runtimeId);Slot* selected=nullptr;
            for(std::size_t n=0;n<32;++n) {
                auto& slot=slots_[(first+n)%capacity];
                if(slot.epoch!=epoch || slot.box.runtimeId==sample.runtimeId) {selected=&slot;break;}
            }
            if(!selected || (selected->epoch==epoch && selected->box.seenAt>=sample.seenAt))continue;
            BoxRecord next;
            static_cast<Sample&>(next)=sample;next.corners=corners(sample.bounds);
            next.blendFrom=sample.bounds;
            if(selected->epoch==epoch && selected->box.runtimeId==sample.runtimeId &&
               sample.seenAt-selected->box.seenAt<=std::chrono::milliseconds(250)) {
                const auto from=interpolatedBounds(selected->box,sample.seenAt);
                if(distanceSquared(center(from),center(sample.bounds))<=64.f) {
                    next.blendFrom=from;
                    next.blendDuration=std::chrono::milliseconds(50); // One 20Hz tick, never a long tail.
                }
            }
            *selected={epoch,next};
        }
    }
    void apply(std::uint64_t epoch,BoxRecord* boxes,std::size_t count) const noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock)return;
        for(std::size_t i=0;i<count;++i) {
            auto& box=boxes[i];const auto first=bucket(box.runtimeId);
            for(std::size_t n=0;n<32;++n) {
                const auto& slot=slots_[(first+n)%capacity];
                if(slot.epoch!=epoch)break;
                if(slot.box.runtimeId!=box.runtimeId)continue;
                if(slot.box.seenAt>=box.seenAt && (!box.retained || slot.box.seenAt>box.seenAt)) {
                    const bool wasPlayer=box.isPlayer;
                    box=slot.box;box.isPlayer=box.isPlayer||wasPlayer;
                }
                break;
            }
        }
    }
private:
    static constexpr std::size_t capacity=4096;
    static std::size_t bucket(std::uint64_t id) noexcept {
        id=(id^(id>>30))*0xbf58476d1ce4e5b9ULL;
        return static_cast<std::size_t>(id^(id>>27))%capacity;
    }
    struct Slot {std::uint64_t epoch{};BoxRecord box;};
    mutable std::mutex mutex_;
    std::array<Slot,capacity> slots_{};
};
}
