#include "../src/collection_geometry.hpp"
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

    // No pickup, however long the sweep runs, cannot release collection.
    int stable = 0;
    for (int tick = 0; tick < 1000; ++tick)
        assert(!pickupConfirmed(0, stable));
    assert(!pickupConfirmed(1, stable));
    assert(!pickupConfirmed(0, stable));
    for (int tick = 0; tick < 7; ++tick)
        assert(!pickupConfirmed(1, stable));
    assert(pickupConfirmed(1, stable));
}
