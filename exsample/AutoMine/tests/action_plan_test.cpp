#include "../src/action_plan.hpp"
#include <cassert>
#include <map>
#include <tuple>

int main() {
    using namespace automine;
    ActionPlan work;
    prioritizeWork(work, Action::Dig, {1,2,3});
    assert(work.actions.size()==1 && work.actions[0].cell==PlanCell(1,2,3));
    work.add(Action::Place,{1,2,3});
    work.add(Action::Walk,{0,1,0});
    prioritizeWork(work, Action::Dig, {1,2,3});
    assert(work.actions.size()==3 && work.actions[0].kind==Action::Dig);
    assert(work.actions[1].kind==Action::Place && work.actions[2].kind==Action::Walk);
    using Key = std::tuple<int,int,int>;
    std::map<Key,Terrain> world;
    auto read = [&](PlanCell p) {
        auto it = world.find({p.x,p.y,p.z});
        return it == world.end() ? Terrain::Unknown : it->second;
    };
    for (int y = 0; y < 10; ++y) world[{0,y,0}] = Terrain::Solid;
    auto shaft = shaftPlan({0,10,0},0,read);
    assert(shaft.complete && shaft.actions.size() == 20);
    for (int i = 0; i < 10; ++i) {
        assert(shaft.actions[i*2].kind == Action::Dig);
        assert(shaft.actions[i*2+1].kind == Action::Walk);
        assert(shaft.actions[i*2].cell.y == 9-i);
        assert(shaft.actions[i*2].cell == shaft.actions[i*2+1].cell);
    }
    // A lava boundary must be visible as the end of confirmed planning,
    // without suggesting movement through it toward the requested depth.
    world[{0,5,0}] = Terrain::Hazard;
    shaft = shaftPlan({0,10,0},0,read);
    assert(!shaft.complete && shaft.actions.size() == 8);
    assert(shaft.actions.back().cell.y == 6);
    world.erase({0,7,0});
    assert(shaftPlan({0,10,0},0,read).actions.size() == 4);
    world.clear();
    for (int x = 1; x <= 6; ++x)
        for (int y = 4; y <= 6; ++y) world[{x,y,0}] = Terrain::Solid;
    auto tunnel = tunnelPlan({0,5,0},1,0,6,read);
    assert(tunnel.complete && tunnel.actions.size() == 18);
    for (int i = 0; i < 6; ++i) {
        assert(tunnel.actions[i*3].cell == (PlanCell{i+1,6,0}));
        assert(tunnel.actions[i*3+1].cell == (PlanCell{i+1,5,0}));
        assert(tunnel.actions[i*3+2].kind == Action::Walk);
    }
    world[{3,4,0}] = Terrain::Air; // a cave interrupts the planned tunnel
    tunnel = tunnelPlan({0,5,0},1,0,6,read);
    assert(!tunnel.complete && tunnel.actions.size() == 6);
    world[{1,6,0}] = Terrain::Unbreakable;
    assert(tunnelPlan({0,5,0},1,0,6,read).actions.empty());
    // Collection follows a supported surface around a wall; a straight
    // line through the wall would fail these collision invariants.
    world.clear();
    for (int x=-1;x<=4;++x) for(int z=-2;z<=2;++z) {
        world[{x,0,z}]=Terrain::Solid;
        for (int y=1;y<=3;++y) world[{x,y,z}]=Terrain::Air;
    }
    world[{1,1,0}]=world[{1,2,0}]=world[{1,3,0}]=Terrain::Solid;
    auto route=surfaceRoute({0,1,0},{3,1,0},read);
    assert(route.size()>4 && route.front()==(PlanCell{0,1,0}) && route.back()==(PlanCell{3,1,0}));
    for (std::size_t i=1;i<route.size();++i) {
        const auto p=route[i], q=route[i-1];
        assert(std::abs(p.x-q.x)+std::abs(p.z-q.z)==1);
        assert(read(p)==Terrain::Air && read({p.x,p.y+1,p.z})==Terrain::Air);
    }
    // One block up requires takeoff headroom; a low ceiling forbids it.
    world.clear();
    world[{0,0,0}]=world[{1,1,0}]=Terrain::Solid;
    for (int y=1;y<=3;++y) world[{0,y,0}]=Terrain::Air;
    world[{1,2,0}]=world[{1,3,0}]=Terrain::Air;
    assert(surfaceRoute({0,1,0},{1,2,0},read).size()==2);
    world[{0,3,0}]=Terrain::Solid;
    assert(surfaceRoute({0,1,0},{1,2,0},read).empty());
    // A drop must have a clear fall corridor and a known landing floor.
    // Four blocks of descent is the collection case for a drop below a
    // temporary pillar/upper ledge.
    world.clear();world[{0,0,0}]=world[{1,-4,0}]=Terrain::Solid;
    world[{0,1,0}]=world[{0,2,0}]=Terrain::Air;
    for(int y=-3;y<=2;++y)world[{1,y,0}]=Terrain::Air;
    assert(surfaceRoute({0,1,0},{1,-3,0},read).size()==2);
    world[{1,0,0}]=Terrain::Hazard;
    assert(surfaceRoute({0,1,0},{1,-3,0},read).empty());

    world.clear();
    world[{0,3,0}] = Terrain::Solid;
    world[{0,4,0}] = Terrain::Air;
    auto climb = pillarReturnPlan({0,1,0},3,read);
    assert(climb.complete && climb.actions.size() == 5);
    assert(climb.actions[0].kind == Action::Dig);
    assert(climb.actions[1].kind == Action::Jump);
    assert(climb.actions[2].kind == Action::Place);
    assert(climb.actions.back().cell == (PlanCell{0,2,0}));
    world[{0,4,0}] = Terrain::Hazard;
    climb = pillarReturnPlan({0,1,0},3,read);
    assert(!climb.complete && climb.actions.size() == 3);

}
