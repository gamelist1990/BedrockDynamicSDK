#include "hitbox/box_cache.hpp"
#include "hitbox/world_render_profile.hpp"
#include <bedrock/generated_symbols.hpp>
#include <cassert>
#include <limits>

int main() {
    hitbox::WorldRenderProfile profile;
    std::string error;
    assert(hitbox::parseWorldRenderProfile(bedrock::generated::kSymbolDatabase,profile,error));
    assert(profile.functions[0].rva==0x47f8570 && profile.functions[0].guard.size()==24);
    assert(profile.commonMaterialGroup==0x11e3b308 && profile.material==0x10f0 && profile.origin==0x660 && profile.tess==0xb8);
    const auto original=nlohmann::json::parse(bedrock::generated::kSymbolDatabase);
    for (const char* key : {"level_renderer_player","render_origin","selection_material",
        "screen_tessellator","screen_shader_color","tess_translation","tess_building",
        "tess_special_mode","shader_color_dirty"}) {
        auto invalid=original; invalid["native_world_render"]["offsets"].erase(key);
        assert(!hitbox::parseWorldRenderProfile(invalid.dump(),profile,error));
    }
    auto invalid=original; invalid.erase("native_world_render");
    assert(!hitbox::parseWorldRenderProfile(invalid.dump(),profile,error));
    invalid=original; invalid["native_world_render"]["functions"]["begin"]["rva"]="0xffffffff";
    assert(!hitbox::parseWorldRenderProfile(invalid.dump(),profile,error));
    invalid=original; invalid["native_world_render"]["functions"]["color"]["entry_check"]="?? ??";
    assert(!hitbox::parseWorldRenderProfile(invalid.dump(),profile,error));
    invalid=original; invalid["native_world_render"]["pointer_size"]=4;
    assert(!hitbox::parseWorldRenderProfile(invalid.dump(),profile,error));
    invalid=original; invalid["native_world_render"].erase("common_material_group_rva");
    assert(!hitbox::parseWorldRenderProfile(invalid.dump(),profile,error));
    for(const char* key:{"hashed_name_ctor","material_ctor"}) {
        invalid=original; invalid["native_world_render"]["functions"].erase(key);
        assert(!hitbox::parseWorldRenderProfile(invalid.dump(),profile,error));
    }
    using namespace std::chrono_literals;
    const auto opaque=hitbox::opaqueColor(0x0044aaee);
    assert(opaque[0]==238/255.f && opaque[1]==170/255.f && opaque[2]==68/255.f && opaque[3]==1.f);
    assert(hitbox::opaqueColor(0x0144aaee)==hitbox::opaqueColor(0xff44aaee));
    hitbox::CaptureSchedule schedule;
    static_assert(hitbox::CaptureSchedule::periodTicks==5);
    std::size_t visited=0;
    for(int tick=0;tick<5;++tick) {
        const auto quota=schedule.beginTick(94);
        assert(quota<=19); visited+=quota; schedule.visited(quota);
    }
    assert(visited==94 && schedule.beginTick(94)==19);
    schedule.reset();
    for(int tick=0;tick<5;++tick) {
        const auto quota=schedule.beginTick(2);
        assert(quota==(tick<2 ? 1u : 0u)); schedule.visited(quota);
    }
    assert(schedule.beginTick(2)==1);
    schedule.reset();
    for(int tick=0;tick<12;++tick) {
        const auto quota=schedule.beginTick(94); assert(quota==19);
        schedule.visited(1); // A tight time budget preserves pending work across cycles.
    }
    schedule.reset(); assert(schedule.beginTick(0)==0);
    assert(schedule.beginTick(1000)==hitbox::CaptureSchedule::maximumBatch);
    const auto now=hitbox::Clock::now();
    const bedrock::Vec3 camera{};
    hitbox::Sample sample{0x100000001ULL, {{0,0,0},{1,2,1}}, now};
    hitbox::BoxCache cache;
    assert(cache.beginWorld(1));
    cache.observe(sample,camera,32,false);
    auto rows=cache.visible(now,camera,32,false,10,1500ms);
    assert(rows.size()==1 && rows[0].runtimeId==sample.runtimeId);
    const auto vertices = hitbox::worldLineVertices(rows[0], {100, 200, 300});
    assert(vertices.size() == 24);
    assert(vertices[0].x == -100 && vertices[0].y == -200 && vertices[0].z == -300);
    assert(vertices[1].x == -99 && vertices[1].y == -200 && vertices[1].z == -300);
    // Last line joins the upper/lower fourth corner in the same coordinate system.
    assert(vertices[22].x == -100 && vertices[22].y == -200 && vertices[22].z == -299);
    assert(vertices[23].x == -100 && vertices[23].y == -198 && vertices[23].z == -299);
    {
        hitbox::BoxCache smooth;
        smooth.beginWorld(1); smooth.observe(sample,camera,32,false);
        auto moved=sample; moved.seenAt=now+500ms;
        moved.bounds={{2,0,0},{3,2,1}};
        smooth.observe(moved,camera,32,false);
        auto boxes=smooth.visible(now+500ms,camera,32,false,10,1500ms);
        assert(hitbox::interpolatedBounds(boxes[0],now+500ms).minimum.x==0);
        assert(hitbox::interpolatedBounds(boxes[0],now+750ms).minimum.x==1);
        assert(hitbox::interpolatedBounds(boxes[0],now+1000ms).minimum.x==2);
        assert(hitbox::interpolatedBounds(boxes[0],now+2000ms).minimum.x==2); // No extrapolation.
        const auto lines=hitbox::worldLineVertices(boxes[0],{},now+750ms);
        assert(lines[0].x==1 && lines[1].x==2);
        // A new sample arriving during a blend starts at the currently drawn position.
        moved.seenAt=now+750ms; moved.bounds={{4,0,0},{5,2,1}};
        smooth.observe(moved,camera,32,false);
        boxes=smooth.visible(now+750ms,camera,32,false,10,1500ms);
        assert(hitbox::interpolatedBounds(boxes[0],now+750ms).minimum.x==1);
        moved.seenAt=now+1000ms; moved.bounds={{20,0,0},{21,2,1}};
        smooth.observe(moved,camera,32,false);
        boxes=smooth.visible(now+1000ms,camera,32,false,10,1500ms);
        assert(boxes[0].blendDuration.count()==0); // Teleport snaps immediately.
        moved.seenAt=now+4000ms; moved.bounds={{21,0,0},{22,2,1}};
        smooth.observe(moved,camera,32,false);
        boxes=smooth.visible(now+4000ms,camera,32,false,10,1500ms);
        assert(boxes[0].blendDuration.count()==0); // Stale IDs do not animate across a gap.
        smooth.beginWorld(2); smooth.observe(sample,camera,32,false);
        boxes=smooth.visible(now,camera,32,false,10,1500ms);
        assert(boxes[0].blendDuration.count()==0);
    }
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
