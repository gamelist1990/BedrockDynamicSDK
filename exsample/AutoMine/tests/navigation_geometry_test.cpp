#include "../src/navigation_geometry.hpp"
#include "../src/locomotion_geometry.hpp"
#include "../src/inventory_policy.hpp"
#include <bedrock/packets/inventory_requests.hpp>
#include <cassert>
#include <map>
#include <tuple>
int main() {
    using namespace automine;
    assert(inMiningReach({.5,2.62,.5},{4,1,0}));
    assert(!inMiningReach({.5,2.62,.5},{5,1,0}));
    // r11 BOL: the nearest face was reachable, but the coal centre was ~5m away.
    const NavPoint bolEye{503,94.62,507};
    const PlanCell coal{500,98,506};
    const auto bolHit = miningPoint(bolEye,coal);
    assert(std::hypot(bolHit.x-bolEye.x,bolHit.y-bolEye.y,bolHit.z-bolEye.z) <= miningReach);
    assert(!inMiningReach(bolEye,coal)); // rejected by the NEW centre-distance guard
    assert(!inMiningReach({503.5,94.62,507.5},{500,98,506}));
    assert(inMiningReach({503.5,95.62,507.5},{500,98,506}));
    // r12: the jump enters reach, but the next pillar's floor and landing
    // still need confirmation. A rejected stance is not a valid handoff.
    const auto pillar = [](PlanCell p) {
        return p == PlanCell{497, 93, 506} ? Terrain::Solid : Terrain::Air;
    };
    const NavPoint airborne{497.39, 94.2, 506.53}, landed{497.39, 94., 506.53};
    const auto handoff = [&](NavPoint feet, bool pending, bool rejected) {
        return miningHandoffReady({feet.x, feet.y + 1.62, feet.z}, coal, pending, rejected,
                                  [&] { return supportFraction(feet, pillar) > .5; });
    };
    assert(inMiningReach({airborne.x, airborne.y + 1.62, airborne.z}, coal));
    assert(!handoff(airborne, false, false));
    assert(!handoff(landed, true, false));
    assert(!handoff(landed, false, true));
    assert(handoff(landed, false, false));
    const auto near=miningPoint({.5,2.62,.5},{4,1,0});assert(near.x==4.05 && near.y==1.95);
    auto arc=jumpTrajectory({0,1,0},{4,1,0});assert(arc.size()>35);
    double peak=0.;for (const auto& p:arc) peak=std::max(peak,p.y);
    assert(peak>2.2 && peak<2.3 && arc.back().x==4.5 && arc.back().y==1.);
    assert(jumpTrajectory({0,1,0},{3,2,0}).empty());
    assert(!jumpTrajectory({0,1,0},{2,2,0}).empty());
    assert(jumpTrajectory({0,1,0},{5,1,0}).empty());
    std::map<std::tuple<int,int,int>,Terrain> world;
    for(int x=-2;x<=6;++x) for(int y=-2;y<=5;++y) for(int z=-2;z<=2;++z) world[{x,y,z}]=Terrain::Air;
    for(int x=-2;x<=0;++x) world[{x,0,0}]=Terrain::Solid;
    world[{4,0,0}]=Terrain::Solid;
    const auto read=[&](PlanCell p) { const auto it=world.find({p.x,p.y,p.z});return it==world.end()?Terrain::Unknown:it->second; };
    assert(jumpClear({0,1,0},{4,1,0},read));
    world[{2,2,0}]=Terrain::Solid;assert(!jumpClear({0,1,0},{4,1,0},read));
    world[{2,2,0}]=Terrain::Air;world[{1,3,0}]=Terrain::Solid;
    assert(!jumpClear({0,1,0},{4,1,0},read));
    world[{1,3,0}]=Terrain::Air;world.erase({2,3,0});
    assert(!jumpClear({0,1,0},{4,1,0},read));
    assert(toolSlot("minecraft:iron_sword")==0 && toolSlot("stone_axe")==1);
    assert(toolSlot("diamond_pickaxe")==2 && toolSlot("wooden_shovel")==3);
    assert(toolRank("stone_pickaxe")>toolRank("golden_pickaxe"));
    assert(consumeToolFirst(1,59,1,3,10,2,2)); // low tier before a worn higher tier
    assert(consumeToolFirst(3,5,1,3,100,2,2)); // then worn before fresh
    assert(!consumeToolFirst(3,100,1,3,5,2,1));
    assert(consumeToolFirst(3,5,2,3,5,1,2)); // equal tools keep the selected slot
    assert(!consumeToolFirst(3,5,1,3,5,2,2));
    assert(buildingPriority("dirt") < buildingPriority("cobblestone"));
    assert(buildingPriority("cobblestone") < buildingPriority("minecraft:oak_planks"));
    for (const auto *id : {"oak_log", "stripped_oak_wood", "crimson_stem",
                           "warped_hyphae", "diamond_block", "crafting_table"}) {
        assert(buildingPriority(id) < 0);
    }
    assert(rawBuildingWood("oak_log") && rawBuildingWood("minecraft:warped_stem"));
    assert(!rawBuildingWood("oak_planks"));
    const std::set<std::string> junk{"poppy","diamond","oak_log","dirt","iron_ingot","stick"};
    assert(discardable("minecraft:poppy",9,junk,{}));
    assert(!discardable("poppy",0,junk,{}));
    assert(!discardable("poppy",9,junk,{"poppy"}));
    for (const auto* id:{"diamond","oak_log","dirt","iron_ingot","stick"}) assert(!discardable(id,9,junk,{}));
    const auto payload=bedrock::dropRequestPayload(-101,12,425,16);
    bedrock::WireReader r(payload);assert(r.varuint()==1 && r.zigzag()==-101 && r.varuint()==1);
    assert(r.varuint()==3 && r.u8()==0 && r.u8()==16); // drop, legacy type, count
    assert(r.u8()==12 && !r.boolean() && r.u8()==12 && r.take(4)==std::string("\xa9\x01\x00\x00",4));
    assert(!r.boolean() && r.varuint()==0 && r.take(4)==std::string(4,static_cast<char>(255)) && !r.remaining());
    const auto decoded=bedrock::ProtocolCodec{}.decode(bedrock::PacketId::ItemStackRequest,payload);
    assert(decoded.at("requests").at(0).at("actions").at(0).at("type_id")=="drop");
    bool rejected=false;try { (void)bedrock::dropRequestPayload(-101,36,1,1); } catch (...) { rejected=true; }
    assert(rejected);
}
