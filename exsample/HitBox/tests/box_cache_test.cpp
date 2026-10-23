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
    assert(profile.material==0x10f0 && profile.origin==0x660 && profile.tess==0xb8);
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
    using namespace std::chrono_literals;
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
