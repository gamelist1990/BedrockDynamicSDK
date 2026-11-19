#pragma once
#include "navigation_geometry.hpp"
#include <optional>

namespace automine {
enum class PickupTile { Unknown, Air, Full, Slab, Obstacle };
inline constexpr double pickupMaxDrop = 4.;

// y denotes the top of the floor cell. Without slab state, the actual
// surface can be at either y or y-.5; use the worst case for every edge.
template <class Read> bool pickupStand(PlanCell p, const Read &read) {
    const auto floor = read({p.x, p.y - 1, p.z});
    return (floor == PickupTile::Full || floor == PickupTile::Slab) &&
           read(p) == PickupTile::Air && read({p.x, p.y + 1, p.z}) == PickupTile::Air;
}

template <class Read> bool pickupLanded(NavPoint feet, PlanCell p, const Read &read) {
    return pickupStand(p, read) &&
           (std::abs(feet.y - p.y) < .12 ||
            (read({p.x, p.y - 1, p.z}) == PickupTile::Slab &&
             std::abs(feet.y - (p.y - .5)) < .12));
}

template <class Read> bool pickupEdge(PlanCell from, PlanCell to, const Read &read) {
    if (std::abs(from.x - to.x) + std::abs(from.z - to.z) != 1 ||
        !pickupStand(from, read) || !pickupStand(to, read))
        return false;
    const auto slab = [&](PlanCell p) {
        return read({p.x, p.y - 1, p.z}) == PickupTile::Slab ? .5 : 0.;
    };
    if (from.y - to.y + slab(to) > pickupMaxDrop || to.y - from.y + slab(from) > 1.)
        return false;
    if (to.y > from.y && read({from.x, from.y + 2, from.z}) != PickupTile::Air)
        return false;
    // From the centre of a cell, a cardinal step sweeps only these two
    // columns. The entire destination column down to the floor must be clear.
    for (int y = to.y + 2; y <= from.y + 1; ++y)
        if (read({to.x, y, to.z}) != PickupTile::Air)
            return false;
    return true;
}

template <class Read> std::vector<PlanCell> pickupRoute(PlanCell start, PlanCell goal,
                                                      const Read &read, int budget = 1200) {
    if (!pickupStand(start, read) || !pickupStand(goal, read))
        return {};
    const auto key = [](PlanCell p) { return std::tuple{p.x, p.y, p.z}; };
    std::queue<PlanCell> open;
    open.push(start);
    std::map<decltype(key(start)), PlanCell> parent{{key(start), start}};
    constexpr int dirs[4][2]{{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    while (!open.empty() && budget-- > 0) {
        const auto p = open.front();
        open.pop();
        if (p == goal) {
            std::vector<PlanCell> path{p};
            while (!(path.back() == start))
                path.push_back(parent.at(key(path.back())));
            std::reverse(path.begin(), path.end());
            return path;
        }
        for (const auto &d : dirs)
            for (int dy : {0, 1, -1, -2, -3, -4}) {
                const PlanCell n{p.x + d[0], p.y + dy, p.z + d[1]};
                if (std::abs(n.x - start.x) > 10 || std::abs(n.z - start.z) > 10 ||
                    std::abs(n.y - start.y) > 8 || parent.count(key(n)) || !pickupEdge(p, n, read))
                    continue;
                parent.emplace(key(n), p);
                open.push(n);
            }
    }
    return {};
}

template <class Read> std::vector<PlanCell> pickupSweep(PlanCell ore, PlanCell start,
                                                      const Read &read) {
    std::vector<PlanCell> sweep;
    for (int dx = -2; dx <= 2; ++dx)
        for (int dz = -2; dz <= 2; ++dz)
            for (int y = ore.y; y >= ore.y - 12; --y) {
                const PlanCell floor{ore.x + dx, y - 1, ore.z + dz};
                if (read(floor) == PickupTile::Air)
                    continue;
                // First surface below the break; don't guess through an
                // obstruction or unloaded terrain.
                const PlanCell p{floor.x, y, floor.z};
                if (pickupStand(p, read))
                    sweep.push_back(p);
                break;
            }
    const auto cost = [&](PlanCell p) {
        return 3 * std::hypot(p.x - ore.x, p.z - ore.z) +
               std::hypot(p.x - start.x, p.y - start.y, p.z - start.z);
    };
    std::stable_sort(sweep.begin(), sweep.end(),
                     [&](PlanCell a, PlanCell b) { return cost(a) < cost(b); });
    return sweep;
}

// Only consider taking the support out from under the player when it is
// ours and doing so leads back onto an already-supported, clear stance.
template <class Read, class Owned>
std::optional<PlanCell> pickupUndercut(PlanCell start, const Read &read, const Owned &owned) {
    const PlanCell floor{start.x, start.y - 1, start.z};
    if (!pickupStand(start, read) || read(floor) != PickupTile::Full || !owned(floor))
        return {};
    const auto after = [&](PlanCell p) { return p == floor ? PickupTile::Air : read(p); };
    if (!pickupStand(floor, after))
        return {};
    return floor;
}

inline bool pickupConfirmed(int gain, int &stableTicks) {
    if (gain <= 0)
        stableTicks = 0;
    else
        ++stableTicks;
    return stableTicks >= 8;
}
} // namespace automine
