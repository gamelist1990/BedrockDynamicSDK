#include "../src/locomotion_geometry.hpp"
#include <cassert>
#include <map>
#include <tuple>

int main() {
    using namespace automine;
    using Key = std::tuple<int, int, int>;
    std::map<Key, Terrain> world;
    auto clear = [&] {
        world.clear();
        for (int x = -5; x <= 8; ++x)
            for (int y = -1; y <= 6; ++y)
                for (int z = -4; z <= 4; ++z)
                    world[{x, y, z}] = y == 0 ? Terrain::Solid : Terrain::Air;
    };
    const auto read = [&](PlanCell p) {
        const auto found = world.find({p.x, p.y, p.z});
        return found == world.end() ? Terrain::Unknown : found->second;
    };
    int reads = 0;
    TerrainCache cache([&](PlanCell p) {
        ++reads;
        return read(p);
    });
    const auto cached = [&](PlanCell p) { return cache(p); };
    (void)cached({0, 0, 0});
    (void)cached({0, 0, 0});
    assert(reads == 1);
    clear();
    assert(walkClear({.5, 1, .5}, {2.5, 1, 2.5}, read));
    assert(groundTrajectory({.5, 1, .5}, {2.5, 1, 2.5}, read).size() > 10);
    world[{1, 1, 0}] = world[{1, 2, 0}] = Terrain::Solid;
    assert(!walkClear({.5, 1, .5}, {1.5, 1, 1.5}, read)); // body clips the inside corner
    clear();
    for (int z = -4; z <= 4; ++z)
        world[{1, 0, z}] = Terrain::Air;
    assert(!walkClear({.5, 1, .5}, {2.5, 1, .5}, read)); // no shortcut across an unfinished floor
    assert(walkClear({.9, 1, .5}, {.5, 1, .5}, read)); // retreat from an overhang remains possible
    assert(supportFraction({.9, 1, .5}, read) > .65 && supportFraction({.9, 1, .5}, read) < .68);
    assert(!fullySupported({.9, 1, .5}, read));
    assert(groundTrajectory({.5, 1, .5}, {2.5, 1, .5}, read).empty());
    clear();
    world[{1, 1, 0}] = Terrain::Solid;
    assert(jumpClear({0, 1, 0}, {1, 2, 0}, read)); // rise before entering the adjacent step
    world[{0, 3, 0}] = Terrain::Solid;
    assert(!jumpClear({0, 1, 0}, {1, 2, 0}, read));
    clear();
    const auto pillar = jumpTrajectory({0, 1, 0}, {0, 2, 0});
    assert(!pillar.empty() && pillar.back().y == 2 && pillar.back().x == .5);
    for (int x = 1; x <= 3; ++x)
        world[{x, 0, 0}] = Terrain::Air;
    assert(jumpClear({0, 1, 0}, {4, 1, 0}, read));
    world[{2, 3, 0}] = Terrain::Solid;
    assert(!jumpClear({0, 1, 0}, {4, 1, 0}, read));
    clear();
    world[{0, 2, 0}] = Terrain::Solid;
    const auto fall = dropTrajectory({.5, 3, .5}, {1.5, 1, .5});
    assert(fall.size() > 10 && fall.back().y == 1);
    for (const auto &point : fall)
        assert(bodyClear(point, read));
    assert(fall[1].x > 1.3 && fall[1].y == 3);
    clear();
    world[{2, 0, 0}] = Terrain::Air;
    const auto anchor = [&](PlanCell c) { return c == PlanCell{3, 0, 0}; };
    const auto faces = placementFaces({2, 0, 0}, {.5, 2.62, .5}, read, anchor);
    assert(faces.size() == 1 && faces[0].face == 4 && faces[0].hit.x == 3);
    assert(placementFaces({2, 0, 0}, {-3.5, 2.62, .5}, read, anchor).empty());
    world[{1, 2, 0}] = Terrain::Solid;
    assert(placementFaces({2, 0, 0}, {.5, 2.62, .5}, read, anchor).empty());
    clear();
    world[{2, 0, 0}] = Terrain::Air;
    const auto hidden = [&](PlanCell c) { return c == PlanCell{1, 0, 0}; };
    assert(
        placementFaces({2, 0, 0}, {.5, 2.62, .5}, read, hidden).empty()); // back face is occluded
    clear();
    world[{4, 0, 0}] = Terrain::Air;
    assert(landingPadReachable({0, 1, 0}, {4, 1, 0}, read,
                               [&](PlanCell c) { return c == PlanCell{5, 0, 0}; }));
    for (int gap = 1; gap <= 3; ++gap)
        assert(jumpRunup({0, 1, 0}, {gap + 1, 1, 0}) >= 1);
    assert(jumpRunup({0, 1, 0}, {4, 1, 0}) == 2);
    assert(sprintTakeoffReady(true, .15, .20, 2));
    assert(!sprintTakeoffReady(true, .15, .20, 1)); // sprint must precede jump
    assert(!sprintTakeoffReady(true, .15, .09, 10)); // no walking takeoff
    assert(!sprintTakeoffReady(false, .15, .20, 10));
    assert(!sprintTakeoffReady(true, -.1, .20, 10));
    assert(!sprintTakeoffReady(true, .4, .20, 10)); // no jump after leaving the edge
    for (const int stage : {0, 3}) {
        const auto drive = parkourDrive(stage);
        assert(!drive.forward && !drive.sprint && !drive.jump);
    }
    const auto run = parkourDrive(1);
    assert(run.forward && run.sprint && !run.jump);
    for (unsigned tick = 0; tick < 30; ++tick) {
        const auto air = parkourDrive(2, tick);
        assert(air.forward && air.sprint); // includes the last .8m before landing
        assert(air.jump == (tick < 2));
    }
    assert(fullCubeSupport("minecraft:bedrock") && fullCubeSupport("cobblestone"));
    for (const auto *id :
          {"stone_slab", "oak_stairs", "oak_fence", "iron_bars", "cactus", "dirt_path"})
        assert(!fullCubeSupport(id));
    clear();
    const PlanCell station{1,1,0};
    const auto allowed = [](PlanCell) { return true; };
    auto stationFaces = workstationFaces(station,{.5,2.62,.5},read,allowed);
    assert(!stationFaces.empty() && stationFaces.front().face == 1);
    world[{1,0,0}] = Terrain::Air; // r9 ground=air must never be a candidate
    assert(workstationFaces(station,{.5,2.62,.5},read,allowed).empty());
    world[{1,0,0}] = Terrain::Solid;
    assert(workstationFaces(station,{.5,2.62,.5},read,
                              [](PlanCell) { return false; }).empty()); // interactive/partial support
    world[{1,1,0}] = Terrain::Solid;
    assert(workstationFaces(station,{.5,2.62,.5},read,allowed).empty());
    world[{1,1,0}] = Terrain::Air; // a completed dug pocket
    assert(!workstationFaces(station,{.5,2.62,.5},read,allowed).empty());
}
