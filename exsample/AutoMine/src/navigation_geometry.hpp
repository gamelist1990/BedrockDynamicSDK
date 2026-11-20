#pragma once
#include "action_plan.hpp"
#include <algorithm>
#include <cmath>
#include <vector>
namespace automine {
struct NavPoint {
    double x{}, y{}, z{};
};
inline constexpr double miningReach = 4.0;
inline constexpr double miningCenterReach = 4.5;
inline NavPoint miningPoint(NavPoint eye, PlanCell cell) {
    return {std::clamp(eye.x, cell.x + .05, cell.x + .95),
            std::clamp(eye.y, cell.y + .05, cell.y + .95),
            std::clamp(eye.z, cell.z + .05, cell.z + .95)};
}
inline bool inMiningReach(NavPoint eye, PlanCell cell) {
    const auto p = miningPoint(eye, cell);
    return std::hypot(cell.x + .5 - eye.x, cell.y + .5 - eye.y, cell.z + .5 - eye.z) <=
               miningCenterReach &&
           std::hypot(p.x - eye.x, p.y - eye.y, p.z - eye.z) <= miningReach;
}
inline constexpr bool onWorkstationColumn(PlanCell stance, PlanCell table) {
    return stance.x == table.x && stance.z == table.z && stance.y >= table.y - 1;
}
template <class Visible>
bool workstationReachable(NavPoint feet, PlanCell table, Visible visible) {
    const PlanCell stance{static_cast<int>(std::floor(feet.x)), static_cast<int>(std::floor(feet.y)),
                          static_cast<int>(std::floor(feet.z))};
    const NavPoint eye{feet.x, feet.y + 1.62, feet.z};
    return !onWorkstationColumn(stance, table) && inMiningReach(eye, table) && visible(eye);
}

struct RouteBudget {
    int perSegment, segments;
};
inline constexpr RouteBudget routeBudget(bool interaction) {
    return interaction ? RouteBudget{1200, 1} : RouteBudget{4096, 2};
}
template <class Supported>
bool miningHandoffReady(NavPoint eye, PlanCell cell, bool constructionPending,
                        bool rejectedStance, Supported supported) {
    return !constructionPending && !rejectedStance && inMiningReach(eye, cell) && supported();
}
// Conservative vanilla jump envelope. Horizontal steering is checked against
// the descending landing time, rather than assuming every four-cell gap works.
inline std::vector<NavPoint> jumpTrajectory(PlanCell from, PlanCell to) {
    const double dx = to.x - from.x, dz = to.z - from.z, d = std::hypot(dx, dz);
    const int rise = to.y - from.y;
    if ((d < .5 && rise != 1) || d > 4.01 || rise > 1 || rise < -1 || (rise > 0 && d > 2.01))
        return {};
    std::vector<double> height{0.};
    double y = 0., v = .42, landingTime = 0.;
    for (int tick = 1; tick <= 20; ++tick) {
        const double before = y;
        y += v;
        v = (v - .08) * .98;
        height.push_back(y);
        if (v < 0 && y <= rise && before > rise) {
            landingTime = tick - 1 + (before - rise) / (before - y);
            break;
        }
    }
    if (landingTime <= 0 || (d - .28) / landingTime > .34)
        return {};
    const double advance = rise > 0 ? (d < 1.5 ? 0. : .18) : .28;
    const NavPoint launch{from.x + .5 + (d > .001 ? dx / d * advance : 0),
                          static_cast<double>(from.y),
                          from.z + .5 + (d > .001 ? dz / d * advance : 0)};
    double delay = 0;
    if (rise > 0 && d > .5 && d < 1.5)
        for (std::size_t i = 1; i < height.size(); ++i)
            if (height[i] >= rise) {
                delay = i - 1 + (rise - height[i - 1]) / (height[i] - height[i - 1]);
                break;
            }
    std::vector<NavPoint> points;
    const int count = static_cast<int>(std::ceil(landingTime * 4));
    for (int i = 0; i <= count; ++i) {
        const double t = landingTime * i / count;
        const int a = static_cast<int>(t);
        const double f = t - a;
        const double h = a + 1 < static_cast<int>(height.size())
                             ? height[a] + (height[a + 1] - height[a]) * f
                             : rise;
        const double u = std::clamp((t - delay) / (landingTime - delay), 0., 1.);
        points.push_back({launch.x + (to.x + .5 - launch.x) * u, from.y + h,
                          launch.z + (to.z + .5 - launch.z) * u});
    }
    points.back() = {to.x + .5, static_cast<double>(to.y), to.z + .5};
    return points;
}
template <class Read> bool jumpClear(PlanCell from, PlanCell to, Read read) {
    const auto arc = jumpTrajectory(from, to);
    if (arc.empty())
        return false;
    for (const auto &p : arc)
        for (int x = static_cast<int>(std::floor(p.x - .29));
             x <= static_cast<int>(std::floor(p.x + .29)); ++x)
            for (int z = static_cast<int>(std::floor(p.z - .29));
                 z <= static_cast<int>(std::floor(p.z + .29)); ++z)
                for (int y = static_cast<int>(std::floor(p.y + .01));
                     y <= static_cast<int>(std::floor(p.y + 1.79)); ++y)
                    if (read({x, y, z}) != Terrain::Air)
                        return false;
    return true;
}
} // namespace automine
