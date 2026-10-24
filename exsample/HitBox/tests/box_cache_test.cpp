#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif
#include "hitbox/box_cache.hpp"
#include "hitbox/batch_queue.hpp"
#include "hitbox/name_tags.hpp"
#ifdef _WIN32
#include "hitbox/name_mesh_win32.hpp"
#endif
#include "hitbox/world_render_profile.hpp"
#include <bedrock/generated_symbols.hpp>
#include <cassert>
#include <limits>

int main() {
    {
        const std::string japanese=reinterpret_cast<const char*>(u8"テスト名前");
        assert(hitbox::normalizedName("  Player  ")=="Player");
        assert(hitbox::normalizedName("\xc2\xa7" "aPlayer\xc2\xa7" "r\nHealth: 20")=="Player");
        assert(hitbox::normalizedName("\xc2\xa7" "b"+japanese)==japanese);
        assert(hitbox::normalizedName("\xc2\xa7" "a   ").empty());
        assert(hitbox::normalizedName(std::string("A\xf0\x9f",3))=="A");
        assert(hitbox::normalizedName(std::string("A\xc0\xaf",3))=="A");
        assert(hitbox::normalizedName(std::string(200,'x')).size()==96);
        auto mailbox=std::make_unique<hitbox::NameMailbox>();
        const hitbox::NameKey key{1,0x10000,42};
        assert(!mailbox->read(key));
        assert(mailbox->observe(key,"Player"));
        const auto first=mailbox->read(key);
        assert(first && std::string(first->bytes.data(),first->size)=="Player");
        assert(!mailbox->observe(key,""));
        assert(!mailbox->observe(key,"Player"));
        assert(mailbox->read(key)->revision==first->revision);
        assert(!mailbox->read({2,key.actor,key.entity})); // World change cannot reuse the name.
        assert(!mailbox->read({1,key.actor,key.entity+0x40000})); // New EnTT generation at same pointer.
        assert(!mailbox->read({1,key.actor+8,key.entity}));
        assert(mailbox->observe(key,japanese));
        const auto updated=mailbox->read(key);
        assert(updated && updated->revision>first->revision);
        assert(hitbox::normalizedName(std::string_view(updated->bytes.data(),updated->size))==japanese);
        assert(mailbox->observe({2,key.actor,key.entity},"NewWorld"));
        assert(std::string(mailbox->read(key)->bytes.data(),mailbox->read(key)->size)==japanese);
        const bedrock::ActorBounds bounds{{-0.5f,0,-0.5f},{0.5f,2,0.5f}};
        const auto label=hitbox::nameBillboard(bounds,{0,5,5});
        const bedrock::Vec3 toCamera{-label.anchor.x,5-label.anchor.y,5-label.anchor.z};
        assert(std::abs(label.up.x*toCamera.x+label.up.y*toCamera.y+label.up.z*toCamera.z)<0.0001f);
        assert(std::abs(label.right.x*label.up.x+label.right.y*label.up.y+label.right.z*label.up.z)<0.0001f);
        const auto vertex=hitbox::nameVertex(label,{0,0},0.25f,{100,200,300});
        assert(vertex.x==-100 && std::abs(vertex.y+197.8f)<0.0001f && vertex.z==-300);
#ifdef _WIN32
        hitbox::NameMesher mesher;
        assert(!mesher.make(""));
        for(const auto& name: {std::string("Player_123"),japanese}) {
            const auto mesh=mesher.make(name);
            assert(mesh && mesh->text==name && mesh->width>0);
            assert(!mesh->triangles.empty() && mesh->triangles.size()%3==0);
            assert(mesh->triangles.size()<=hitbox::NameTriangleSink::maximumTriangles*3);
            for(const auto point:mesh->triangles) assert(std::isfinite(point.x) && std::isfinite(point.y));
        }
#endif
    }
    hitbox::WorldRenderProfile profile;
    std::string error;
    assert(hitbox::parseWorldRenderProfile(bedrock::generated::kSymbolDatabase,profile,error));
    assert(profile.actorTypeValueOffset==0 && profile.playerTypeId==319);
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
    for(const char* key:{"value_offset","value_size","player_type_id"}) {
        invalid=original; invalid["native_actors"]["actor_type"].erase(key);
        assert(!hitbox::parseWorldRenderProfile(invalid.dump(),profile,error));
    }
    invalid=original; invalid["native_actors"]["actor_type"]["value_size"]=8;
    assert(!hitbox::parseWorldRenderProfile(invalid.dump(),profile,error));
    hitbox::BatchQueue<int,2> queue;
    assert(queue.empty() && !queue.pop());
    assert(queue.push(1) && queue.push(2) && !queue.push(3));
    assert(queue.pop()==1 && queue.push(3));
    assert(queue.pop()==2 && queue.pop()==3 && queue.empty());
    hitbox::PendingDelivery<int,2> delivery;
    unsigned sent[2]{};
    assert(delivery.begin(42));
    assert(!delivery.flush([&](std::size_t shard,int value){assert(value==42); ++sent[shard]; return shard==0;}));
    assert(!delivery.begin(99)); // New captures cannot overwrite a blocked delivery.
    assert(delivery.flush([&](std::size_t shard,int value){assert(value==42 && shard==1); ++sent[shard];return true;}));
    assert(sent[0]==1 && sent[1]==2); // Accepted shard is never sent a duplicate.
    assert(delivery.begin(99)); delivery.clear();
    assert(delivery.flush([](auto,auto){assert(false);return false;})); // Drop only at a world reset.
    using namespace std::chrono_literals;
    const auto opaque=hitbox::opaqueColor(0x0044aaee);
    assert(opaque[0]==238/255.f && opaque[1]==170/255.f && opaque[2]==68/255.f && opaque[3]==1.f);
    assert(hitbox::opaqueColor(0x0144aaee)==hitbox::opaqueColor(0xff44aaee));
    hitbox::CaptureSchedule schedule;
    static_assert(hitbox::CaptureSchedule::periodTicks==1);
    std::size_t visited=0;
    for(int tick=0;tick<3;++tick) {
        const auto quota=schedule.beginTick(94);
        assert(quota==(tick<2 ? 32u : 30u)); visited+=quota; schedule.visited(quota);
    }
    assert(visited==94 && schedule.beginTick(94)==32);
    schedule.reset();
    for(int tick=0;tick<5;++tick) {
        const auto quota=schedule.beginTick(2);
        assert(quota==2); schedule.visited(quota);
    }
    schedule.reset();
    for(int tick=0;tick<12;++tick) {
        const auto quota=schedule.beginTick(94); assert(quota==32);
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
    auto rows=cache.visible(now,camera,32,false,1500ms);
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
        auto boxes=smooth.visible(now+500ms,camera,32,false,1500ms);
        assert(hitbox::interpolatedBounds(boxes[0],now+500ms).minimum.x==0);
        assert(hitbox::interpolatedBounds(boxes[0],now+750ms).minimum.x==1);
        assert(hitbox::interpolatedBounds(boxes[0],now+1000ms).minimum.x==2);
        assert(hitbox::interpolatedBounds(boxes[0],now+2000ms).minimum.x==2); // No extrapolation.
        const auto lines=hitbox::worldLineVertices(boxes[0],{},now+750ms);
        assert(lines[0].x==1 && lines[1].x==2);
        // A new sample arriving during a blend starts at the currently drawn position.
        moved.seenAt=now+750ms; moved.bounds={{4,0,0},{5,2,1}};
        smooth.observe(moved,camera,32,false);
        boxes=smooth.visible(now+750ms,camera,32,false,1500ms);
        assert(hitbox::interpolatedBounds(boxes[0],now+750ms).minimum.x==1);
        moved.seenAt=now+1000ms; moved.bounds={{20,0,0},{21,2,1}};
        smooth.observe(moved,camera,32,false);
        boxes=smooth.visible(now+1000ms,camera,32,false,1500ms);
        assert(boxes[0].blendDuration.count()==0); // Teleport snaps immediately.
        moved.seenAt=now+4000ms; moved.bounds={{21,0,0},{22,2,1}};
        smooth.observe(moved,camera,32,false);
        boxes=smooth.visible(now+4000ms,camera,32,false,1500ms);
        assert(boxes[0].blendDuration.count()==0); // Stale IDs do not animate across a gap.
        smooth.beginWorld(2); smooth.observe(sample,camera,32,false);
        boxes=smooth.visible(now,camera,32,false,1500ms);
        assert(boxes[0].blendDuration.count()==0);
    }
    {
        hitbox::BoxCache all;
        all.beginWorld(1);
        for(std::uint64_t id=1;id<=1025;++id) {
            auto actor=sample; actor.runtimeId=id; actor.isPlayer=(id%2==1);
            actor.bounds={{1000+static_cast<float>(id),0,0},{1001+static_cast<float>(id),2,1}};
            all.observe(actor,camera,32,false,true);
        }
        assert(all.visible(now,camera,32,false,1500ms,true).size()==1000);
        const auto players=all.visible(now,camera,32,false,1500ms,true,true);
        assert(players.size()==500);
        for(const auto& player:players) assert(player.isPlayer && player.runtimeId%2==1);
        assert(all.visible(now,camera,32,false,1500ms,true,false).size()==1000);
        assert(all.visible(now,camera,32,false,1500ms,false).empty()); // Optional distance filter.
        assert(all.visible(now,camera,32,false,1500ms,true).size()==1000); // Not evicted by view filter.
    }
    // Shortened native batches must not flicker out at the old 750ms expiry.
    assert(cache.visible(now+1000ms,camera,32,false,1500ms).size()==1);
    assert(cache.visible(now+1600ms,camera,32,false,1500ms).empty());

    {
        const auto budget=std::make_shared<hitbox::TrackingBudget>();
        hitbox::BoxCache first{budget},second{budget};
        assert(first.beginWorld(1) && second.beginWorld(1));
        // A skewed RuntimeID distribution can use all 1000 slots on one worker.
        for(std::uint64_t id=1;id<=1000;++id) {
            auto actor=sample; actor.runtimeId=id*2;
            first.observe(actor,camera,100,false);
        }
        auto other=sample; other.runtimeId=9999;
        second.observe(other,camera,100,false);
        assert(first.size()==1000 && second.size()==0 && budget->size()==1000);
        first.visible(now+2s,camera,100,false,1500ms);
        second.observe(other,camera,100,false);
        assert(budget->size()==1 && second.size()==1);
        // Old epoch releases cannot consume or free the new world's slots.
        assert(first.beginWorld(2));
        other.seenAt=now+3s; first.observe(other,camera,100,false);
        assert(budget->size()==1);
        second.visible(now+5s,camera,100,false,1500ms);
        assert(budget->size()==1 && !second.beginWorld(1));
        assert(second.beginWorld(2));
    }
    {
        const auto budget=std::make_shared<hitbox::TrackingBudget>();
        hitbox::BoxCache saved{budget}; saved.beginWorld(1);
        auto player=sample; player.isPlayer=true;
        for(int i=0;i<=8;++i) {
            player.seenAt=now+250ms*i; saved.observe(player,camera,100,true);
        }
        assert(saved.size()==1 && saved.retainedSize()==1 && budget->size()==1);
        saved.visible(now+4s,camera,100,true,1500ms);
        assert(saved.size()==0 && saved.retainedSize()==1 && budget->size()==1);
        saved.visible(now+5s,camera,100,false,1500ms);
        assert(budget->size()==0); // Disabling a saved-only record returns its slot.
        for(int i=0;i<=8;++i) {
            player.seenAt=now+6s+250ms*i; saved.observe(player,camera,100,true);
        }
        saved.visible(now+10min+8s,camera,100,true,1500ms);
        assert(budget->size()==0 && saved.retainedSize()==0);
    }
    {
        hitbox::PlayerIdentityCache identities;
        assert(!identities.classify(0,true));
        assert(!identities.classify(42,false));
        assert(identities.classify(42,true));
        assert(identities.classify(42,false)); // Same RuntimeID survives ActorType loss/change.
        assert(!identities.classify(43,false)); // A replacement ID cannot inherit Player identity.
        identities.reset();
        assert(!identities.known(42));
        for(std::uint64_t id=1;id<=hitbox::PlayerIdentityCache::maximumPlayers;++id)
            assert(identities.classify(id,true));
        const auto extra=hitbox::PlayerIdentityCache::maximumPlayers+1;
        assert(identities.classify(extra,true)); // Live positive classification still works at capacity.
        assert(!identities.classify(extra,false)); // Bounded history does not evict known Players.
        assert(identities.known(1));
    }
    {
        hitbox::BoxCache latest;
        latest.beginWorld(1);
        auto player=sample; player.isPlayer=true;
        latest.observe(player,camera,32,true);
        assert(latest.retainedSize()==1); // Save the first valid Player observation immediately.
        auto boxes=latest.visible(now,camera,32,true,1500ms,false,true);
        assert(boxes.size()==1 && !boxes[0].retained); // No duplicate retained box while live.
        player.bounds={{2,0,0},{3,2,1}}; player.seenAt=now+250ms;
        latest.observe(player,camera,32,true);
        boxes=latest.visible(now+2s,camera,32,true,1500ms,false,true);
        assert(boxes.size()==1 && boxes[0].retained && boxes[0].bounds.minimum.x==2);
        assert(boxes[0].blendDuration.count()==0); // Missing Actor shows exact last bounds, no extrapolation.
        const auto frozen=boxes[0];
        assert(hitbox::visibleInFrame(frozen,player.seenAt+10min-1ms,1500ms,true,true));
        assert(!hitbox::visibleInFrame(frozen,player.seenAt+10min,1500ms,true,true));
        // A delayed batch must not rewind the saved position after live expiry.
        auto delayed=sample; delayed.isPlayer=true; delayed.seenAt=now+100ms;
        latest.observe(delayed,camera,32,true);
        boxes=latest.visible(now+2s,camera,32,true,1500ms);
        assert(boxes.size()==1 && boxes[0].bounds.minimum.x==2);
        // Same ID with a changed/missing ActorType keeps its Player classification and updates position.
        player.isPlayer=false; player.bounds={{5,0,0},{6,2,1}}; player.seenAt=now+3s;
        latest.observe(player,camera,32,true);
        boxes=latest.visible(now+3s,camera,32,true,1500ms,false,true);
        assert(boxes.size()==1 && boxes[0].isPlayer && !boxes[0].retained && boxes[0].bounds.minimum.x==5);
        // A zero-sized or non-finite replacement shape is never recorded as a live box.
        auto collapsed=player; collapsed.seenAt=now+4s; collapsed.bounds.maximum=collapsed.bounds.minimum;
        latest.observe(collapsed,camera,32,true);
        boxes=latest.visible(now+5s,camera,32,true,1500ms,false,true);
        assert(boxes.size()==1 && boxes[0].retained && boxes[0].bounds.minimum.x==5);
        assert(latest.visible(now+3s+10min,camera,32,true,1500ms).empty());
        assert(latest.retainedSize()==0);
        player.seenAt=now+11min; player.isPlayer=true;
        latest.observe(player,camera,32,true);
        assert(latest.retainedSize()==1);
        assert(!latest.beginWorld(0) && latest.retainedSize()==1);
        assert(latest.beginWorld(2) && latest.retainedSize()==0 && latest.size()==0);
    }
    {
        hitbox::BoxCache ranged;
        ranged.beginWorld(1);
        auto edge=sample; edge.isPlayer=true;
        edge.bounds={{99.5f,-0.5f,-0.5f},{100.5f,0.5f,0.5f}};
        ranged.observe(edge,camera,100,true);
        assert(ranged.retainedSize()==1);
        assert(ranged.visible(now+2s,camera,100,true,1500ms).size()==1);
        assert(ranged.visible(now+2s,camera,99,true,1500ms).empty());
        assert(ranged.visible(now+10min,camera,99,true,1500ms).empty());
        assert(ranged.retainedSize()==0);
        edge.bounds={{100,-0.5f,-0.5f},{101,0.5f,0.5f}}; edge.seenAt=now+11min;
        ranged.observe(edge,camera,100,true);
        assert(ranged.size()==0 && ranged.retainedSize()==0);
    }
    {
        hitbox::BoxCache moving,npc;
        moving.beginWorld(1); npc.beginWorld(1);
        auto player=sample; player.isPlayer=true;
        for(int i=0;i<=20;++i) {
            player.seenAt=now+250ms*i;
            player.bounds={{0.04f*i,0,0},{1+0.04f*i,2,1}};
            moving.observe(player,camera,32,true); // Motion continuously refreshes the same record.
            auto other=sample; other.seenAt=player.seenAt;
            npc.observe(other,camera,32,true);
        }
        assert(moving.retainedSize()==1 && npc.retainedSize()==0);
        auto boxes=moving.visible(now+7s,camera,32,true,1500ms);
        assert(boxes.size()==1 && boxes[0].retained && boxes[0].bounds.minimum.x==player.bounds.minimum.x);
        moving.visible(now+7s,camera,32,false,1500ms);
        assert(moving.retainedSize()==0);
        player.seenAt=now+8s;
        moving.observe(player,camera,32,true);
        assert(moving.retainedSize()==1); // Re-enabling starts saving from the next valid sample.
        moving.visible(now+8s,camera,32,true,1500ms,false,true,1s);
        assert(moving.retainedSize()==1);
        player.seenAt=now+9s;
        moving.observe(player,camera,32,true);
        assert(moving.visible(now+9s,camera,32,true,1500ms,false,true,1s).size()==1);
        assert(moving.retainedSize()==1); // Fresh observations renew expiry without a new delay.
    }

    cache.observe(sample,camera,32,false);
    // Out-of-order samples cannot overwrite a more recent capture.
    auto older=sample;
    older.seenAt=now-1s;
    older.bounds.minimum.x=0.5f;
    cache.observe(older,camera,32,false);
    assert(cache.visible(now,camera,32,false,1500ms)[0].bounds.minimum.x==0);
    assert(cache.beginWorld(2) && cache.size()==0);
    assert(!cache.beginWorld(1) && cache.size()==0); // Reject an old world's delayed batch.

    sample.bounds.minimum.x=std::numeric_limits<float>::quiet_NaN();
    cache.observe(sample,camera,32,false);
    assert(cache.size()==0);
    sample.bounds={{0,0,0},{1,2,1}};
    // At capacity new IDs are rejected; existing tracking is not churned.
    for (std::size_t i=0;i<=hitbox::BoxCache::maximumRecords;++i) {
        sample.runtimeId=i+1;
        sample.seenAt=now+std::chrono::milliseconds(i);
        cache.observe(sample,camera,32,true);
    }
    assert(cache.size()==hitbox::BoxCache::maximumRecords);
    rows=cache.visible(now+1000ms,camera,32,true,1500ms);
    assert(rows.size()==hitbox::BoxCache::maximumRecords);
    for (const auto& row:rows) assert(row.runtimeId!=hitbox::BoxCache::maximumRecords+1);
}
