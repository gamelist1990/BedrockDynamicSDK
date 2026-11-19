#pragma once

#include "navigation_geometry.hpp"
#include <utility>
#include <string_view>

namespace automine {
template <class Read> class TerrainCache {
  public:
    explicit TerrainCache(Read read) : read_(std::move(read)) {}
    Terrain operator()(PlanCell cell) {
        const auto key = std::tuple{cell.x, cell.y, cell.z};
        const auto found = cells_.find(key);
        if (found != cells_.end())
            return found->second;
        const auto value = read_(cell);
        if (cells_.size() < 8192)
            cells_.emplace(key, value);
        return value;
    }

  private:
    Read read_;
    std::map<std::tuple<int, int, int>, Terrain> cells_;
};

inline constexpr double playerHalfWidth = .30;
inline constexpr double playerHeight = 1.80;

inline bool fullCubeSupport(std::string_view id) {
    const auto colon = id.find(':');
    if (colon != id.npos)
        id.remove_prefix(colon + 1);
    // Fractional shapes need block-state collision data; do not assume a full cube.
    for (const auto part : {"slab", "stairs", "fence", "wall", "door", "pane", "carpet", "rail",
                            "ladder", "chain", "candle", "torch", "button", "pressure_plate",
                            "flower", "sapling", "snow_layer", "scaffolding"})
        if (id.find(part) != id.npos)
            return false;
    for (const auto special : {"fire",          "soul_fire",     "portal",
                               "nether_portal", "end_portal",    "end_gateway",
                               "web",           "cobweb",        "sweet_berry_bush",
                               "bamboo",        "lily_pad",      "sea_pickle",
                               "turtle_egg",    "brewing_stand", "cauldron",
                               "hopper",        "anvil",         "chest",
                               "trapped_chest", "ender_chest",   "enchanting_table",
                               "end_rod",       "lightning_rod", "stonecutter",
                               "grindstone",    "lectern",       "bell"})
        if (id == special)
            return false;
    return !id.empty() && id != "air" && id != "water" && id != "lava" && id != "bed" &&
           !id.ends_with("_bed") && id != "iron_bars" && id != "farmland" && id != "dirt_path" &&
           id != "soul_sand" && id != "cactus" && id != "campfire" && id != "soul_campfire" &&
           id != "powder_snow";
}

template <class Read> bool bodyClear(NavPoint feet, Read read) {
    constexpr double epsilon = .001;
    for (int x = static_cast<int>(std::floor(feet.x - playerHalfWidth + epsilon));
         x <= static_cast<int>(std::floor(feet.x + playerHalfWidth - epsilon)); ++x)
        for (int z = static_cast<int>(std::floor(feet.z - playerHalfWidth + epsilon));
             z <= static_cast<int>(std::floor(feet.z + playerHalfWidth - epsilon)); ++z)
            for (int y = static_cast<int>(std::floor(feet.y + epsilon));
                 y <= static_cast<int>(std::floor(feet.y + playerHeight - epsilon)); ++y)
                if (read({x, y, z}) != Terrain::Air)
                    return false;
    return true;
}

template <class Read> double supportFraction(NavPoint feet, Read read) {
    if (std::abs(feet.y - std::round(feet.y)) > .10)
        return 0.;
    const int y = static_cast<int>(std::llround(feet.y)) - 1;
    double area = 0.;
    for (int x = static_cast<int>(std::floor(feet.x - playerHalfWidth));
         x <= static_cast<int>(std::floor(feet.x + playerHalfWidth)); ++x)
        for (int z = static_cast<int>(std::floor(feet.z - playerHalfWidth));
             z <= static_cast<int>(std::floor(feet.z + playerHalfWidth)); ++z) {
            const auto cell = read({x, y, z});
            if (cell != Terrain::Solid && cell != Terrain::Unbreakable)
                continue;
            const double width =
                std::max(0., std::min(feet.x + playerHalfWidth, x + 1.) -
                                 std::max(feet.x - playerHalfWidth, static_cast<double>(x)));
            const double depth =
                std::max(0., std::min(feet.z + playerHalfWidth, z + 1.) -
                                 std::max(feet.z - playerHalfWidth, static_cast<double>(z)));
            area += width * depth;
        }
    return std::clamp(area / (4 * playerHalfWidth * playerHalfWidth), 0., 1.);
}

template <class Read> bool fullySupported(NavPoint feet, Read read) {
    if (std::abs(feet.y - std::round(feet.y)) > .10)
        return false;
    const int y = static_cast<int>(std::llround(feet.y)) - 1;
    for (int x = static_cast<int>(std::floor(feet.x - playerHalfWidth + .001));
         x <= static_cast<int>(std::floor(feet.x + playerHalfWidth - .001)); ++x)
        for (int z = static_cast<int>(std::floor(feet.z - playerHalfWidth + .001));
             z <= static_cast<int>(std::floor(feet.z + playerHalfWidth - .001)); ++z) {
            const auto cell = read({x, y, z});
            if (cell != Terrain::Solid && cell != Terrain::Unbreakable)
                return false;
        }
    return true;
}

template <class Read> bool walkClear(NavPoint from, NavPoint to, Read read) {
    if (std::abs(from.y - to.y) > .10)
        return false;
    const int samples =
        std::max(1, static_cast<int>(std::ceil(std::hypot(to.x - from.x, to.z - from.z) / .12)));
    bool regained = fullySupported(from, read);
    for (int i = 0; i <= samples; ++i) {
        const double t = static_cast<double>(i) / samples;
        const NavPoint p{from.x + (to.x - from.x) * t, from.y, from.z + (to.z - from.z) * t};
        if (!bodyClear(p, read))
            return false;
        if (fullySupported(p, read))
            regained = true;
        else if (regained || supportFraction(p, read) < .3)
            return false;
    }
    return regained && fullySupported(to, read);
}

inline int jumpRunup(PlanCell from, PlanCell to) {
    const double d = std::hypot(to.x - from.x, to.z - from.z);
    // Even a one-block gap is a sprint jump, never a standing walking jump.
    return d > 3.5 ? 2 : 1;
}

inline bool sprintTakeoffReady(bool grounded, double along, double speed,
                               unsigned long long sprintCalls) {
    return grounded && along >= .08 && along <= .38 && speed >= .12 && sprintCalls >= 2;
}

struct ParkourDrive { bool forward{}, sprint{}, jump{}; };
inline ParkourDrive parkourDrive(int stage, unsigned long long flightTicks = 0) {
    const bool driving = stage == 1 || stage == 2;
    return {driving, driving, stage == 2 && flightTicks < 2};
}

inline std::vector<NavPoint> dropTrajectory(NavPoint from, NavPoint to) {
    if (to.y >= from.y)
        return {from, to};
    const double length = std::hypot(to.x - from.x, to.z - from.z);
    if (length < .001)
        return {from, to};
    // Clear the supporting block with the whole body before lowering the feet.
    const double leave = std::clamp((.5 + playerHalfWidth + .01) / length, 0., 1.);
    const NavPoint edge{from.x + (to.x - from.x) * leave, from.y, from.z + (to.z - from.z) * leave};
    std::vector<NavPoint> result{from, edge};
    const double fall = from.y - to.y;
    const int samples = std::max(8, static_cast<int>(std::ceil(std::sqrt(fall / .04) * 4)));
    for (int i = 1; i <= samples; ++i) {
        const double t = static_cast<double>(i) / samples;
        result.push_back(
            {edge.x + (to.x - edge.x) * t, from.y - fall * t * t, edge.z + (to.z - edge.z) * t});
    }
    return result;
}

// A planned walking edge is drawn only where its swept body and support exist.
template <class Read>
std::vector<NavPoint> groundTrajectory(NavPoint from, NavPoint to, Read read) {
    if (std::abs(from.y - to.y) > .10)
        return {};
    if (!walkClear(from, to, read))
        return {};
    std::vector<NavPoint> points;
    const int samples =
        std::max(1, static_cast<int>(std::ceil(std::hypot(to.x - from.x, to.z - from.z) / .2)));
    for (int i = 0; i <= samples; ++i) {
        const double t = static_cast<double>(i) / samples;
        points.push_back({from.x + (to.x - from.x) * t, from.y, from.z + (to.z - from.z) * t});
    }
    return points;
}
inline constexpr double placementReach = 4.1;
struct PlacementFace {
    PlanCell anchor;
    NavPoint hit;
    unsigned face{};
    double distance{};
};
template <class Read> bool visibleFace(NavPoint eye, NavPoint hit, Read read) {
    const double length = std::hypot(hit.x - eye.x, hit.y - eye.y, hit.z - eye.z);
    const int count = std::max(1, static_cast<int>(std::ceil(length / .04)));
    for (int i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / count;
        if (read({static_cast<int>(std::floor(eye.x + (hit.x - eye.x) * t)),
                  static_cast<int>(std::floor(eye.y + (hit.y - eye.y) * t)),
                  static_cast<int>(std::floor(eye.z + (hit.z - eye.z) * t))}) != Terrain::Air)
            return false;
    }
    return true;
}
template <class Read, class Anchor>
std::vector<PlacementFace> placementFaces(PlanCell floor, NavPoint eye, Read read,
                                          Anchor anchorAllowed) {
    std::vector<PlacementFace> options;
    if (read(floor) != Terrain::Air)
        return options;
    constexpr int dirs[6][4]{{1, 0, 0, 4},  {-1, 0, 0, 5}, {0, 0, 1, 2},
                             {0, 0, -1, 3}, {0, -1, 0, 1}, {0, 1, 0, 0}};
    for (const auto &d : dirs) {
        const PlanCell anchor{floor.x + d[0], floor.y + d[1], floor.z + d[2]};
        if (!anchorAllowed(anchor))
            continue;
        NavPoint hit{std::clamp(eye.x, anchor.x + .05, anchor.x + .95),
                     std::clamp(eye.y, anchor.y + .05, anchor.y + .95),
                     std::clamp(eye.z, anchor.z + .05, anchor.z + .95)};
        if (d[0])
            hit.x = anchor.x + (d[0] > 0 ? 0. : 1.);
        if (d[1])
            hit.y = anchor.y + (d[1] > 0 ? 0. : 1.);
        if (d[2])
            hit.z = anchor.z + (d[2] > 0 ? 0. : 1.);
        const double distance = std::hypot(hit.x - eye.x, hit.y - eye.y, hit.z - eye.z);
        if (distance <= placementReach && visibleFace(eye, hit, read))
            options.push_back({anchor, hit, static_cast<unsigned>(d[3]), distance});
    }
    std::sort(options.begin(), options.end(),
              [](const auto &a, const auto &b) { return a.distance < b.distance; });
    return options;
}
template <class Read, class Support>
std::vector<PlacementFace> workstationFaces(PlanCell cell, NavPoint eye, Read read,
                                           Support supportAllowed) {
    const PlanCell support{cell.x, cell.y - 1, cell.z};
    const auto ground = read(support);
    if ((ground != Terrain::Solid && ground != Terrain::Unbreakable) ||
        !supportAllowed(support))
        return {};
    return placementFaces(cell, eye, read,
                           [&](PlanCell p) { return p == support; });
}
template <class Read, class Anchor>
bool landingPadReachable(PlanCell from, PlanCell to, Read read, Anchor anchorAllowed) {
    const PlanCell floor{to.x, to.y - 1, to.z};
    const auto arc = jumpTrajectory(from, to);
    for (std::size_t i = 0; i < arc.size() && i * 5 <= arc.size() * 3; i += 2) {
        auto eye = arc[i];
        eye.y += 1.62;
        if (!placementFaces(floor, eye, read, anchorAllowed).empty())
            return true;
    }
    return false;
}

} // namespace automine
