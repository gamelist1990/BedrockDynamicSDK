#include "hitbox/box_cache.hpp"
#include <cassert>
#include <limits>

int main() {
    using namespace std::chrono_literals;
    const auto now=hitbox::Clock::now();
    const bedrock::Vec3 camera{};
    hitbox::Sample sample{0x100000001ULL, {{0,0,0},{1,2,1}}, now};
    hitbox::BoxCache cache;
    assert(cache.beginWorld(1));
    cache.observe(sample,camera,32,false);
    auto rows=cache.visible(now,camera,32,false,10,1500ms);
    assert(rows.size()==1 && rows[0].runtimeId==sample.runtimeId);
    // Shortened native batches must not flicker out at the old 750ms expiry.
    assert(cache.visible(now+1000ms,camera,32,false,10,1500ms).size()==1);
    assert(cache.visible(now+1600ms,camera,32,false,10,1500ms).empty());

    cache.observe(sample,camera,32,true);
    auto far=sample;
    far.bounds={{100,0,0},{101,2,1}};
    far.seenAt=now+1s;
    cache.observe(far,camera,32,true);
    rows=cache.visible(now+5s,camera,32,true,10,1500ms);
    assert(rows.size()==1 && rows[0].bounds.minimum.x==0); // Retain the last near position.
    assert(cache.visible(now+5s,{100,0,0},4,true,10,1500ms).empty());
    assert(cache.visible(now+11min,camera,32,true,10,1500ms).empty());

    cache.observe(sample,camera,32,false);
    // Out-of-order samples cannot overwrite a more recent capture.
    auto older=sample;
    older.seenAt=now-1s;
    older.bounds.minimum.x=0.5f;
    cache.observe(older,camera,32,false);
    assert(cache.visible(now,camera,32,false,10,1500ms)[0].bounds.minimum.x==0);
    assert(cache.beginWorld(2) && cache.size()==0);
    assert(!cache.beginWorld(1) && cache.size()==0); // Reject an old world's delayed batch.

    sample.bounds.minimum.x=std::numeric_limits<float>::quiet_NaN();
    cache.observe(sample,camera,32,false);
    assert(cache.size()==0);
    sample.bounds={{0,0,0},{1,2,1}};
    // Retention memory remains bounded even across a long series of unique IDs.
    for (std::size_t i=0;i<=hitbox::BoxCache::maximumRecords;++i) {
        sample.runtimeId=i+1;
        sample.seenAt=now+std::chrono::milliseconds(i);
        cache.observe(sample,camera,32,true);
    }
    assert(cache.size()==hitbox::BoxCache::maximumRecords);
    rows=cache.visible(now+10s,camera,32,true,10,1500ms);
    for (const auto& row:rows) assert(row.runtimeId!=1);
}
