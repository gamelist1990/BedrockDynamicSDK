#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include <map>
#include <queue>
#include <tuple>

namespace automine {
struct PlanCell { int x{}, y{}, z{}; bool operator==(const PlanCell&) const = default; };
enum class Action { Walk, Dig, Place, Jump, Collect };
enum class Terrain { Unknown, Air, Solid, Hazard, Unbreakable };
struct PlanAction { Action kind; PlanCell cell; };
struct ActionPlan {
    std::vector<PlanAction> actions;
    bool complete{true};
    void add(Action kind, PlanCell cell) { actions.push_back({kind, cell}); }
};

// Keep immediate work visible even when its owning phase has no movement route.
inline void prioritizeWork(ActionPlan &plan, Action kind, PlanCell cell) {
    std::erase_if(plan.actions, [&](const PlanAction &action) {
        return action.kind == kind && action.cell == cell;
    });
    plan.actions.insert(plan.actions.begin(), {kind, cell});
}

// A pillar return is work followed by movement, not an invisible phase.
// Stop at unknown/hazardous terrain rather than previewing through it.
template<class Read>
ActionPlan pillarReturnPlan(PlanCell feet, int targetY, Read read) {
    ActionPlan plan;
    for (int y = feet.y; y < std::min(targetY, feet.y + 64); ++y) {
        const PlanCell cap{feet.x, y + 2, feet.z};
        const auto head = read(cap);
        if (head != Terrain::Air && head != Terrain::Solid) {
            plan.complete = false;
            break;
        }
        if (head == Terrain::Solid) plan.add(Action::Dig, cap);
        plan.add(Action::Jump, {feet.x, y + 1, feet.z});
        plan.add(Action::Place, {feet.x, y, feet.z});
    }
    return plan;
}

// A bounded, terrain-only route for collecting drops. Never invent a direct
// approach through a wall, and never require mining the support under a drop.
template<class Read>
std::vector<PlanCell> surfaceRoute(PlanCell start, PlanCell goal, Read read, int budget = 1200) {
    using Key = std::tuple<int,int,int>;
    const auto key=[](PlanCell p){return Key{p.x,p.y,p.z};};
    const auto stand=[&](PlanCell p) {
        const auto floor=read({p.x,p.y-1,p.z});
        return (floor==Terrain::Solid||floor==Terrain::Unbreakable) &&
            read(p)==Terrain::Air && read({p.x,p.y+1,p.z})==Terrain::Air;
    };
    if (!stand(goal)) return {};
    std::queue<PlanCell> open; open.push(start);
    std::map<Key,PlanCell> parent; parent.emplace(key(start),start);
    constexpr int dirs[4][2]{{1,0},{-1,0},{0,1},{0,-1}};
    while (!open.empty() && budget-->0) {
        const auto p=open.front();open.pop();
        if (p==goal) {
            std::vector<PlanCell> path{p};
            while (!(path.back()==start)) path.push_back(parent.at(key(path.back())));
            std::reverse(path.begin(),path.end());return path;
        }
        // A drop can land four blocks below a pillar/upper ledge.  Treat that
        // as a normal walkable descent; the executor still requires every
        // intermediate column to be clear and supported.
        for (const auto& d:dirs) for (int dy:{0,1,-1,-2,-3,-4}) {
            const PlanCell n{p.x+d[0],p.y+dy,p.z+d[1]};
            if (std::abs(n.x-start.x)>10||std::abs(n.z-start.z)>10||
                std::abs(n.y-start.y)>8||parent.count(key(n))||!stand(n)) continue;
            if (dy>0 && read({p.x,p.y+2,p.z})!=Terrain::Air) continue;
            bool clear=true;
            for (int y=n.y+2;y<=p.y+1;++y)
                if (read({n.x,y,n.z})!=Terrain::Air) clear=false;
            if (!clear) continue;
            parent.emplace(key(n),p);open.push(n);
        }
    }
    return {};
}

// Plan the complete known shaft, including each excavation BEFORE its fall.
// Unknown chunks and hazardous cells end the confirmed plan at that boundary.
template<class Read>
ActionPlan shaftPlan(PlanCell feet, int targetY, Read read) {
    ActionPlan plan;
    targetY = std::max(targetY, -64);
    int fall = 0;
    for (int y = feet.y - 1; y >= targetY; --y) {
        const PlanCell cell{feet.x, y, feet.z};
        const auto terrain = read(cell);
        if (terrain == Terrain::Unknown || terrain == Terrain::Hazard ||
            terrain == Terrain::Unbreakable) { plan.complete = false; break; }
        if (terrain == Terrain::Solid) { plan.add(Action::Dig, cell); fall = 0; }
        else if (++fall > 7) { plan.complete = false; break; }
        plan.add(Action::Walk, cell);
    }
    return plan;
}

template<class Read>
ActionPlan tunnelPlan(PlanCell feet, int dx, int dz, int length, Read read) {
    ActionPlan plan;
    for (int i = 1; i <= std::clamp(length, 0, 32); ++i) {
        const PlanCell cell{feet.x + dx * i, feet.y, feet.z + dz * i};
        const auto floor = read({cell.x, cell.y - 1, cell.z});
        if (floor != Terrain::Solid && floor != Terrain::Unbreakable) {
            plan.complete = false; break;
        }
        // Match the executor: head first, then feet, then walk.
        for (int y : {cell.y + 1, cell.y}) {
            const PlanCell dig{cell.x, y, cell.z};
            const auto terrain = read(dig);
            if (terrain == Terrain::Unknown || terrain == Terrain::Hazard ||
                terrain == Terrain::Unbreakable) { plan.complete = false; return plan; }
            if (terrain == Terrain::Solid) plan.add(Action::Dig, dig);
        }
        plan.add(Action::Walk, cell);
    }
    return plan;
}
} // namespace automine
