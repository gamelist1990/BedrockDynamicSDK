#include "../src/collection_geometry.hpp"
#include "../src/block_knowledge.hpp"
#include <cassert>

int main() {
    using namespace automine;
    using Key = std::tuple<int, int, int>;
    std::map<Key, PickupTile> world;
    const auto read = [&](PlanCell p) {
        const auto it = world.find({p.x, p.y, p.z});
        return it == world.end() ? PickupTile::Unknown : it->second;
    };
    const PlanCell ore{500, 98, 506}, pillar{497, 94, 506}, goal{500, 90, 506};
    for (int x = 497; x <= 502; ++x)
        for (int y = 88; y <= 99; ++y)
            world[{x, y, 506}] = PickupTile::Air;
    for (int x = 497; x <= 499; ++x)
        world[{x, 89, 506}] = PickupTile::Full;
    for (int y = 90; y <= 93; ++y)
        world[{497, y, 506}] = PickupTile::Full;
    world[{500, 89, 506}] = PickupTile::Slab;

    // r13: descend four cells next to the pillar, then walk to the
    // coal under the mined block. Both slab elevations are recognized.
    const PlanCell landing{498, 90, 506};
    const auto sweep = pickupSweep(ore, pillar, read);
    assert(!sweep.empty() && sweep.front() == goal);
    const auto route = pickupRoute(pillar, sweep.front(), read);
    assert(route.size() == 4 && route[1] == landing && route.back() == goal);
    assert(pickupLanded({500.5, 89.5, 506.5}, goal, read));
    assert(pickupLanded({500.5, 90., 506.5}, goal, read));
    assert(!pickupLanded({500.5, 90.25, 506.5}, goal, read));

    world.erase({498, 92, 506});
    assert(pickupRoute(pillar, goal, read).empty()); // unknown corridor
    world[{498, 92, 506}] = PickupTile::Obstacle;
    assert(pickupRoute(pillar, goal, read).empty()); // hazard or partial obstruction
    world[{498, 92, 506}] = PickupTile::Air;
    world[{498, 89, 506}] = PickupTile::Obstacle;
    assert(pickupRoute(pillar, goal, read).empty()); // unsafe landing

    // A bottom slab may make the drop exceed four blocks. Shorten the
    // tower by one owned block rather than assuming the slab is on top.
    world[{498, 89, 506}] = PickupTile::Slab;
    assert(pickupRoute(pillar, goal, read).empty());
    const PlanCell floor{497, 93, 506};
    const auto ours = [&](PlanCell p) { return p == floor; };
    assert(!pickupUndercut(pillar, read, [](PlanCell) { return false; }));
    assert(pickupUndercut(pillar, read, ours) == floor);
    world[{497, 92, 506}] = PickupTile::Unknown;
    assert(!pickupUndercut(pillar, read, ours));
    world[{497, 92, 506}] = PickupTile::Full;
    world[{497, 93, 506}] = PickupTile::Air;
    assert(pickupRoute(floor, goal, read).size() == 4);

    // Never return or route to the stance currently occupied by the player.
    assert(pickupRoute(pillar, pillar, read).empty());

    // No pickup, however long the route/arrival checks run, cannot release collection.
    int stable = 0;
    for (int tick = 0; tick < 1000; ++tick)
        assert(!pickupConfirmed(0, stable));
    assert(!pickupConfirmed(1, stable));
    assert(!pickupConfirmed(0, stable));
    for (int tick = 0; tick < 7; ++tick)
        assert(!pickupConfirmed(1, stable));
    assert(pickupConfirmed(1, stable));

    // r18: the physical entity position is the collection target, not the
    // mined block centre.  The normal route target is the item position,
    // and arrival is checked by pickup distance rather than a ring sweep.
    world.clear();
    for (int x = 487; x <= 493; ++x)
        for (int z = 507; z <= 513; ++z)
            for (int y = 88; y <= 94; ++y)
                world[{x, y, z}] = PickupTile::Air;
    for (int x = 487; x <= 493; ++x)
        for (int z = 507; z <= 513; ++z)
            world[{x, 89, z}] = PickupTile::Full;
    const double dropX = 489.77, dropY = 91.125, dropZ = 510.93;
    const auto targetX = static_cast<int>(std::floor(dropX));
    const auto targetY = static_cast<int>(std::floor(dropY));
    const auto targetZ = static_cast<int>(std::floor(dropZ));
    assert(targetX == 489 && targetY == 91 && targetZ == 510);
    const double feetY = 90.125;
    assert(!inPickupReach({dropX, feetY, dropZ}, {dropX, dropY, dropZ}));

    // r19: ItemActor identity/name can be absent or differ from the drop ID.
    // Detection must remain position-based and must not require a name match alone.
    const auto localDropId = [](const std::string &value) {
        const auto colon = value.find(':');
        return colon == std::string::npos ? value : value.substr(colon + 1);
    };
    assert(localDropId("minecraft:acacia_log") == "acacia_log");
    assert(localDropId("acacia_log") == "acacia_log");
    assert(localDropId("").empty());

    // No name or main-hand match: accept only the SDK's dropped-item type.
    const auto nearDrop = pickupCandidateKey(false, 64, 1.5);
    const auto farDrop = pickupCandidateKey(false, 64, 4.0);
    assert(nearDrop && farDrop && *nearDrop < *farDrop);
    assert(!pickupCandidateKey(false, 90, 1.0));
    assert(!pickupCandidateKey(false, std::nullopt, 1.0));
    const auto explicitMatch = pickupCandidateKey(true, std::nullopt, 5.0);
    assert(explicitMatch && *explicitMatch < *nearDrop);
    assert(!pickupCandidateKey(true, 90, 1.0)); // named mobs are not drops

    // r21 field report: 2.8 blocks is mining reach, but not pickup reach.
    const NavPoint fieldItem{-1.006219, 64.125, -11.629424};
    const NavPoint fieldFeet{1.684689, 64., -12.459372};
    assert(inMiningReach({fieldFeet.x, fieldFeet.y + 1.62, fieldFeet.z}, {-2, 64, -12}));
    assert(!inPickupReach(fieldFeet, fieldItem));
    for (auto plant : {"short_grass", "tallgrass", "tall_grass", "large_fern", "dandelion",
                       "cornflower", "red_tulip", "rose_bush", "double_plant"})
        assert(client::isWalkThroughVegetation(plant));
    for (auto solid : {"grass_block", "sunflower_block", "oak_log", "oak_leaves",
                       "flower_pot", "sweet_berry_bush", "cactus"})
        assert(!client::isWalkThroughVegetation(solid));

    const PlanCell plantCell{-2, 64, -12}, besideTrunk{-1, 64, -12}, start{1, 64, -13};
    const auto throughGrass = pickupStances(fieldItem, [&](PlanCell p) {
        return p == start || (p == plantCell && client::isWalkThroughVegetation("tall_grass"));
    });
    assert(throughGrass.size() == 1 && throughGrass.front() == plantCell);
    const auto blockedExact = pickupStances(fieldItem, [&](PlanCell p) {
        return p == start || p == besideTrunk;
    });
    assert(blockedExact.size() == 1 && blockedExact.front() == besideTrunk);
    assert(inPickupReach(pickupStancePoint(besideTrunk, fieldItem), fieldItem));
    // Centre-based arrival alone misses a drop along the far edge of a blocked cell.
    const NavPoint edgeDrop{-1.60, 64.125, -11.629424};
    assert(!inPickupReach({-.5, 64., -11.5}, edgeDrop));
    assert(inPickupReach(pickupStancePoint(besideTrunk, edgeDrop), edgeDrop));
    assert(!pickupArrived({-.65, 64., -11.629424}, edgeDrop, std::nullopt));
    assert(pickupArrived(pickupStancePoint(besideTrunk, edgeDrop), edgeDrop, besideTrunk));
    // The executor still has to move when the chosen stance is the start cell.
    const NavPoint edgeFeet{-.1, 64., -11.1};
    assert(!inPickupReach(edgeFeet, fieldItem));
    assert(inPickupReach(pickupStancePoint(besideTrunk, fieldItem), fieldItem));
    const auto noLanding = pickupStances({fieldItem.x, 69.7, fieldItem.z},
                                        [&](PlanCell p) { return p == plantCell; });
    assert(noLanding.empty()); // airborne drop: wait for a later scan
}
