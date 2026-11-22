#include "auto_mine.hpp"
#include "collection_geometry.hpp"
#include "diagnostics.hpp"

namespace automine::client {

bool AutoMineMod::isPassableCell(const std::string &id) {
    if (id.empty())
        return false;
    if (isAir(id))
        return true;
    const auto l = localPart(id);
    return isWalkThroughVegetation(l) || l == "snow_layer" || l == "sculk_vein" ||
           l == "glow_lichen" || contains(l, "carpet") || l == "torch" || l == "wall_torch";
}

bool AutoMineMod::isFluidCell(const std::string &id) {
    const auto l = localPart(id);
    return contains(l, "water") || contains(l, "lava");
}

bool AutoMineMod::isSolidCell(const std::string &id) {
    return !id.empty() && !isAir(id) && !isFluidCell(id) && !isPassableCell(id);
}

bool AutoMineMod::isDiggableCell(const std::string &id) {
    return isSolidCell(id) && localPart(id) != "bedrock";
}

double AutoMineMod::blockDigCost(const std::string &id) {
    const auto l = localPart(id);
    if (contains(l, "leaves"))
        return 1.5;
    if (l == "dirt" || l == "grass_block" || l == "coarse_dirt" || l == "podzol" ||
        l == "rooted_dirt" || l == "mud" || contains(l, "sand") || l == "gravel" || l == "clay" ||
        l == "snow" || l == "soul_soil")
        return 2.0;
    if (contains(l, "log") || contains(l, "wood") || contains(l, "planks") || l == "crafting_table")
        return 4.0;
    if (contains(l, "deepslate") || l == "tuff" || l == "basalt")
        return 9.0;
    if (l == "obsidian" || l == "crying_obsidian")
        return 80.0;
    return 6.0; // stone family and everything else solid
}

AutoMineMod::CellKind AutoMineMod::classifyCell(CellKey c, CellCache &cache) {
    if (auto it = cache.find(c); it != cache.end())
        return it->second.kind;
    CellKind kind = CellKind::Blocked;
    double digCost = 0.0;
    // Never plan a dig through the chain's crafting table.
    if (chainTablePlaced_ && c.x == chainTablePos_.x && c.z == chainTablePos_.z &&
        (c.y == chainTablePos_.y || c.y + 1 == chainTablePos_.y)) {
        cache.emplace(c, CellInfo{kind, 0.0});
        return kind;
    }
    const auto ground = blockAt({c.x, c.y - 1, c.z});
    const auto feetB = blockAt({c.x, c.y, c.z});
    const auto headB = blockAt({c.x, c.y + 1, c.z});
    if (!ground.empty() && !feetB.empty() && !headB.empty() && !isFluidCell(ground) &&
        !isFluidCell(feetB) && !isFluidCell(headB) && isSolidCell(ground) &&
        automine::fullCubeSupport(ground)) {
        const bool fP = isPassableCell(feetB), hP = isPassableCell(headB);
        if (fP && hP)
            kind = CellKind::Stand;
        else if ((fP || isDiggableCell(feetB)) && (hP || isDiggableCell(headB))) {
            kind = CellKind::Dig;
            digCost = 1.0; // the step itself
            if (!fP)
                digCost += blockDigCost(feetB);
            if (!hP)
                digCost += blockDigCost(headB);
        }
    }
    if (kind == CellKind::Blocked && planSpares_ > 0 && !ground.empty() && isAir(ground) &&
        !feetB.empty() && !headB.empty() && isPassableCell(feetB) && isPassableCell(headB) &&
        !bridgeBlocked_.count({c.x, c.y - 1, c.z})) {
        const BlockPos3 g{c.x, c.y - 1, c.z};
        bool anchor = isSolidCell(blockAt({g.x, g.y - 1, g.z}));
        if (!anchor) {
            constexpr int kD[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto &d : kD) {
                const BlockPos3 n{g.x + d[0], g.y, g.z + d[1]};
                const auto nid = blockAt(n);
                if (isSolidCell(nid)) {
                    anchor = true;
                    break;
                }
                if (!isAir(nid))
                    continue;
                for (const auto &e : kD)
                    if (isSolidCell(blockAt({n.x + e[0], g.y, n.z + e[1]}))) {
                        anchor = true;
                        break;
                    }
                if (anchor)
                    break;
            }
        }
        if (anchor)
            kind = CellKind::Bridge;
    }
    if (planBlocked_.count(c) && kind != CellKind::Blocked) {
        kind = CellKind::Blocked;
        digCost = 0.0;
    }
    cache.emplace(c, CellInfo{kind, digCost});
    return kind;
}

double AutoMineMod::digCostOf(CellKey c, CellCache &cache) {
    classifyCell(c, cache);
    const auto it = cache.find(c);
    return it == cache.end() ? 25.0 : it->second.digCost;
}

bool AutoMineMod::isGravityBlock(const std::string &local) {
    return local == "gravel" || local == "sand" || local == "red_sand" ||
           contains(local, "concrete_powder") || contains(local, "suspicious_");
}

void AutoMineMod::discardPathIfExhausted() {
    if (pathIdx_ >= path_.size()) {
        path_.clear();
        pathDig_.clear();
        pathBridge_.clear();
        pathParkour_.clear();
        pathPillar_.clear();
        pathIdx_ = 0;
        pathPartial_ = false;
    }
}

void AutoMineMod::invalidateRoute() {
    path_.clear();
    pathDig_.clear();
    pathBridge_.clear();
    pathParkour_.clear();
    pathPillar_.clear();
    pathIdx_ = 0;
    pathPartial_ = false;
    pathFor_ = {INT_MIN, 0, 0};
    planFailTick_ = tickCount_ - 200;
    jumpHoldTicks_ = 0;
    parkourWaypoint_ = {INT_MIN, 0, 0};
    pillarWpTicks_ = pillarLandingTicks_ = 0;
    ++pathRevision_;
    releaseInputs();
}

bool AutoMineMod::planPath(bedrock::sdk::Vec3Value feet) {
    crashrec::Section section("planPath");
    ++pathRevision_;
    const bool pickup = phase_ == Phase::Collect;
    const bool workstation = phase_ == Phase::Navigate && navReturn_;
    const bool walkOnly = pickup || workstation;
    const automine::PlanCell targetCell{target_.x, target_.y, target_.z};
    const automine::NavPoint item{collectEntityPos_.x, collectEntityPos_.y, collectEntityPos_.z};
    if (!walkOnly && !(mineRejectedFor_ == target_)) {
        mineRejectedStances_.clear();
        mineRejectedFor_ = target_;
    }
    if (!(planBlockedFor_ == target_)) {
        planBlocked_.clear();
        planBlockedFor_ = target_;
    }
    planSpares_ = walkOnly ? 0 : spareCount();
    CellCache cache;
    const auto stepKind = [&](CellKey p) {
        if (workstation && automine::onWorkstationColumn({p.x, p.y, p.z}, targetCell))
            return CellKind::Blocked;
        const auto kind = classifyCell(p, cache);
        return walkOnly && kind != CellKind::Stand ? CellKind::Blocked : kind;
    };
    std::unordered_map<CellKey, automine::Terrain, CellKeyHash> terrainCache;
    terrainCache.reserve(8192);
    const auto terrainAt = [&](automine::PlanCell p) {
        const CellKey key{p.x, p.y, p.z};
        const auto found = terrainCache.find(key);
        if (found != terrainCache.end())
            return found->second;
        const auto value = planTerrain(p);
        if (terrainCache.size() < 100000)
            terrainCache.emplace(key, value);
        return value;
    };
    CellKey start{static_cast<int>(std::floor(feet.x)), static_cast<int>(std::floor(feet.y)),
                  static_cast<int>(std::floor(feet.z))};
    // Do not teleport the origin of a walking route into a neighbouring goal.
    // If we're airborne the real origin can still generate a safe drop edge.
    if (!walkOnly && classifyCell(start, cache) == CellKind::Blocked) {
        double bestD = 1e18;
        CellKey best = start;
        bool found = false;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    if (!dx && !dy && !dz)
                        continue;
                    const CellKey c{start.x + dx, start.y + dy, start.z + dz};
                    if (classifyCell(c, cache) == CellKind::Blocked)
                        continue;
                    const double d = std::hypot(c.x + .5 - feet.x, c.z + .5 - feet.z) +
                                     std::abs(c.y + .5 - feet.y);
                    if (d < bestD) {
                        bestD = d;
                        best = c;
                        found = true;
                    }
                }
        if (found)
            start = best;
    }
    // Goal acceptance depends on the action, not just the target block.
    std::vector<CellKey> goals;
    bool anyStandGoal = false;
    if (pickup) {
        for (const auto g : automine::pickupStances(item, [&](automine::PlanCell p) {
                 return stepKind({p.x, p.y, p.z}) == CellKind::Stand;
             }))
            goals.push_back({g.x, g.y, g.z});
        anyStandGoal = !goals.empty();
    } else {
        for (int dx = -3; dx <= 3; ++dx)
            for (int dy = -4; dy <= 2; ++dy)
                for (int dz = -3; dz <= 3; ++dz) {
                    const CellKey g{target_.x + dx, target_.y + dy, target_.z + dz};
                    if (workstation) {
                        if (stepKind(g) == CellKind::Stand &&
                            workstationReachableFrom({g.x + .5f, float(g.y), g.z + .5f}, target_))
                            goals.push_back(g);
                        continue;
                    }
                    // Rejected mining stances are still walkable, but cannot be used as endpoints.
                    const bedrock::sdk::Vec3Value goalEye{g.x + .5f, g.y + 1.62f, g.z + .5f};
                    if (mineRejectedStances_.count({g.x, g.y, g.z}) ||
                        !automine::inMiningReach({goalEye.x, goalEye.y, goalEye.z}, targetCell))
                        continue;
                    const auto gk = classifyCell(g, cache);
                    if (gk == CellKind::Blocked) {
                        const bool buildable = !planBlocked_.count(g) && planSpares_ > 0 &&
                                               isAir(blockAt({g.x, g.y - 1, g.z})) &&
                                               isPassableCell(blockAt({g.x, g.y, g.z})) &&
                                               isPassableCell(blockAt({g.x, g.y + 1, g.z}));
                        if (!buildable)
                            continue;
                    }
                    BlockPos3 obstruction;
                    if (!lineOfSight(goalEye, target_, obstruction) &&
                        !isDiggableCell(blockAt(obstruction)))
                        continue;
                    goals.push_back(g);
                    if (gk == CellKind::Stand)
                        anyStandGoal = true;
                }
    }
    if (goals.empty()) {
        note("path: no standpoint near target");
        discardPathIfExhausted();
        return false;
    }
    const double hWeight = walkOnly ? 1.0 : anyStandGoal ? 1.15 : 1.5;
    const double placementCost = 4.5 + 8.0 / (planSpares_ + 1.0);
    int segmentUsed = 0;
    const std::unordered_set<CellKey, CellKeyHash> goalSet(goals.begin(), goals.end());
    std::unordered_map<CellKey, double, CellKeyHash> heuristicCache;
    heuristicCache.reserve(8192);
    auto heuristic = [&](const CellKey &c) {
        if (const auto found = heuristicCache.find(c); found != heuristicCache.end())
            return found->second;
        double bestSquared = 1e18;
        for (const auto &g : goals) {
            const double dx = static_cast<double>(c.x) - g.x, dy = static_cast<double>(c.y) - g.y,
                         dz = static_cast<double>(c.z) - g.z;
            bestSquared = std::min(bestSquared, dx * dx + dy * dy + dz * dz);
        }
        double value = std::sqrt(bestSquared) * hWeight;
        // Break near-ties in favour of a stance closer to the item.
        if (pickup && value == 0.) {
            const auto p = automine::pickupStancePoint({c.x, c.y, c.z}, item);
            value = std::hypot(p.x - item.x, p.y - item.y, p.z - item.z);
        }
        if (heuristicCache.size() < 100000)
            heuristicCache.emplace(c, value);
        return value;
    };
    struct OpenNode {
        double f;
        CellKey c;
        bool operator<(const OpenNode &o) const noexcept {
            return f > o.f;
        } // min-heap
    };
    constexpr int kDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    const auto budget = automine::routeBudget(walkOnly);
    constexpr int kMaxPillarRun = 4; // r11's three-step cap can stop one step short of reach
    int expansions = 0;
    int segments = 0;
    bool partial = false;
    std::vector<CellKey> cells;
    std::unordered_map<CellKey, bool, CellKeyHash> allDig, allBridge, allParkour, allPillar;
    CellKey segStart = start;
    bool reachedGoal = false;
    while (segments < budget.segments) {
        ++segments;
        std::priority_queue<OpenNode> open;
        std::unordered_map<CellKey, double, CellKeyHash> gScore;
        std::unordered_map<CellKey, CellKey, CellKeyHash> cameFrom;
        std::unordered_map<CellKey, bool, CellKeyHash> digFlag;
        std::unordered_map<CellKey, bool, CellKeyHash> bridgeFlag;
        std::unordered_map<CellKey, bool, CellKeyHash> parkourFlag;
        std::unordered_map<CellKey, bool, CellKeyHash> pillarFlag;
        std::unordered_map<CellKey, int, CellKeyHash> placedCount;
        auto flagOf = [](const std::unordered_map<CellKey, bool, CellKeyHash> &m,
                         const CellKey &c) {
            const auto it = m.find(c);
            return it != m.end() && it->second;
        };
        std::unordered_map<CellKey, int, CellKeyHash> pillarRun;
        placedCount[segStart] = segmentUsed;
        gScore[segStart] = 0;
        open.push({heuristic(segStart), segStart});
        int segExpansions = 0;
        CellKey found{};
        bool haveFound = false;
        CellKey bestNode = segStart;
        const double startH = heuristic(segStart);
        double bestH = startH;
        while (!open.empty() && segExpansions < budget.perSegment) {
            const OpenNode cur = open.top();
            open.pop();
            const double cg = gScore.count(cur.c) ? gScore[cur.c] : 1e18;
            if (cur.f > cg + heuristic(cur.c) + 1e-9)
                continue; // stale entry
            ++segExpansions;
            ++expansions;
            {
                const double h = heuristic(cur.c);
                if (h < bestH - 1e-9) {
                    bestH = h;
                    bestNode = cur.c;
                }
            }
            if (goalSet.contains(cur.c)) {
                found = cur.c;
                haveFound = true;
                break;
            }
            auto relax = [&](const CellKey &next, double cost, CellKind kind) {
                const int used = placedCount[cur.c] + (kind == CellKind::Bridge ? 1 : 0);
                if (used > planSpares_)
                    return;
                const double ng = cg + cost;
                if (!gScore.count(next) || ng < gScore[next] - 1e-9) {
                    gScore[next] = ng;
                    cameFrom[next] = cur.c;
                    digFlag[next] = (kind == CellKind::Dig);
                    bridgeFlag[next] = (kind == CellKind::Bridge);
                    parkourFlag[next] = false;
                    pillarFlag[next] = false;
                    placedCount[next] = used;
                    pillarRun[next] = 0;
                    open.push({ng + heuristic(next), next});
                }
            };
            for (const auto &d : kDirs) {
                const int nx = cur.c.x + d[0], nz = cur.c.z + d[1];
                { // walk, same level
                    const CellKey n{nx, cur.c.y, nz};
                    const auto k = stepKind(n);
                    if (k != CellKind::Blocked)
                        relax(n,
                              k == CellKind::Dig      ? digCostOf(n, cache)
                              : k == CellKind::Bridge ? placementCost
                                                      : 1.0,
                              k);
                }
                { // step up one block: the block above our own head
                    const CellKey n{nx, cur.c.y + 1, nz};
                    const auto k = stepKind(n);
                    const auto aboveHead = blockAt({cur.c.x, cur.c.y + 2, cur.c.z});
                    const bool ahPass = isPassableCell(aboveHead);
                    const bool ahDig = !walkOnly && !ahPass && isDiggableCell(aboveHead);
                    const auto stepTerrain = [&](automine::PlanCell p) {
                        if (ahDig && p == automine::PlanCell{cur.c.x, cur.c.y + 2, cur.c.z})
                            return automine::Terrain::Air;
                        if (k == CellKind::Dig && p.x == n.x && p.z == n.z &&
                            (p.y == n.y || p.y == n.y + 1))
                            return automine::Terrain::Air;
                        return terrainAt(p);
                    };
                    if (k != CellKind::Blocked && k != CellKind::Bridge && (ahPass || ahDig) &&
                        automine::jumpClear({cur.c.x, cur.c.y, cur.c.z}, {n.x, n.y, n.z},
                                            stepTerrain)) {
                        double cost = k == CellKind::Dig ? digCostOf(n, cache) + 0.6 : 1.6;
                        if (ahDig)
                            cost += blockDigCost(aboveHead);
                        relax(n, cost, k);
                    }
                }
                // Collection also allows the existing four-block descent.
                if (isPassableCell(blockAt({nx, cur.c.y + 1, nz}))) {
                    for (int drop = 1; drop <= (pickup ? automine::pickupMaxDrop : 3); ++drop) {
                        const CellKey n{nx, cur.c.y - drop, nz};
                        const auto nk = stepKind(n);
                        if (nk == CellKind::Dig) {
                            if (drop == 1)
                                relax(n, digCostOf(n, cache) + 0.3, CellKind::Dig);
                            break;
                        }
                        if (nk != CellKind::Stand) {
                            // An empty intermediate cell is the fall shaft,
                            // not a reason to reject its deeper landing.
                            if (isPassableCell(blockAt({nx, cur.c.y - drop, nz})))
                                continue;
                            break;
                        }
                        bool shaft = true;
                        for (int k2 = 0; k2 < drop; ++k2)
                            if (!isPassableCell(blockAt({nx, cur.c.y - k2, nz}))) {
                                shaft = false;
                                break;
                            }
                        if (!shaft)
                            break;
                        relax(n, 1.0 + 0.3 * drop, CellKind::Stand);
                    }
                }
            }
            for (int dx : {-1, 1})
                for (int dz : {-1, 1}) {
                    const CellKey next{cur.c.x + dx, cur.c.y, cur.c.z + dz};
                    if (classifyCell(cur.c, cache) != CellKind::Stand ||
                        stepKind(next) != CellKind::Stand)
                        continue;
                    if (automine::walkClear(
                            {cur.c.x + .5, static_cast<double>(cur.c.y), cur.c.z + .5},
                            {next.x + .5, static_cast<double>(next.y), next.z + .5}, terrainAt))
                        relax(next, std::sqrt(2.), CellKind::Stand);
                }
            { // dig straight down: the cell below is a Dig cell whose
                const CellKey n{cur.c.x, cur.c.y - 1, cur.c.z};
                if (stepKind(n) == CellKind::Dig)
                    relax(n, digCostOf(n, cache) + 0.3, CellKind::Dig);
            }
            if (planSpares_ > 0 && !flagOf(digFlag, cur.c) &&
                (classifyCell(cur.c, cache) == CellKind::Stand || flagOf(bridgeFlag, cur.c) ||
                 flagOf(pillarFlag, cur.c))) {
                const int pc = placedCount.count(cur.c) ? placedCount[cur.c] : 0;
                if (pillarRun[cur.c] < kMaxPillarRun && pc < planSpares_) {
                    const CellKey n{cur.c.x, cur.c.y + 1, cur.c.z};
                    bool clear = true;
                    double capCost = 0.;
                    for (int y = n.y; y <= n.y + 2; ++y) {
                        const auto id = blockAt({n.x, y, n.z});
                        if (isPassableCell(id))
                            continue;
                        if (!isDiggableCell(id) || isFluidCell(id)) {
                            clear = false;
                            break;
                        }
                        capCost += blockDigCost(id);
                    }
                    if (clear && !planBlocked_.count(n)) {
                        const double ng = cg + placementCost + capCost;
                        if (!gScore.count(n) || ng < gScore[n] - 1e-9) {
                            gScore[n] = ng;
                            cameFrom[n] = cur.c;
                            digFlag[n] = false;
                            bridgeFlag[n] = false;
                            parkourFlag[n] = false;
                            pillarFlag[n] = true;
                            placedCount[n] = pc + 1;
                            pillarRun[n] = pillarRun[cur.c] + 1;
                            open.push({ng + heuristic(n), n});
                        }
                    }
                }
            }
            // Evaluate a swept player body, landing height and run-up,
            // including a landing pad attached to an existing side wall.
            if (!flagOf(digFlag, cur.c) && !flagOf(bridgeFlag, cur.c) &&
                !flagOf(pillarFlag, cur.c)) {
                for (const auto &d : kDirs) {
                    const BlockPos3 first{cur.c.x + d[0], cur.c.y - 1, cur.c.z + d[1]};
                    if (!isAir(blockAt(first)) || blockAt(first).empty())
                        continue;
                    for (int dist = 2; dist <= 4; ++dist)
                        for (int rise : {0, 1, -1}) {
                            const CellKey land{cur.c.x + d[0] * dist, cur.c.y + rise,
                                               cur.c.z + d[1] * dist};
                            const auto kind = stepKind(land);
                            const bool pad = kind == CellKind::Bridge;
                            if (kind != CellKind::Stand && !pad)
                                continue;
                            if (pad && !wallLandingAnchor({land.x, land.y - 1, land.z}))
                                continue;
                            if (pad && !automine::landingPadReachable(
                                           {cur.c.x, cur.c.y, cur.c.z}, {land.x, land.y, land.z},
                                           terrainAt, [&](automine::PlanCell c) {
                                               const auto id = blockAt({c.x, c.y, c.z});
                                               return isSolidCell(id) &&
                                                      automine::fullCubeSupport(id);
                                           }))
                                continue;

                            const int used = placedCount.count(cur.c) ? placedCount[cur.c] : 0;
                            if (pad && used >= planSpares_)
                                continue;
                            const int runup = automine::jumpRunup({cur.c.x, cur.c.y, cur.c.z},
                                                                  {land.x, land.y, land.z});
                            bool runway = true;
                            for (int back = 1; back <= runup; ++back)
                                if (classifyCell(
                                        {cur.c.x - d[0] * back, cur.c.y, cur.c.z - d[1] * back},
                                        cache) != CellKind::Stand)
                                    runway = false;
                            if (runway && runup) {
                                runway = automine::walkClear(
                                    {cur.c.x + .5 - d[0] * runup, static_cast<double>(cur.c.y),
                                     cur.c.z + .5 - d[1] * runup},
                                    {cur.c.x + .5, static_cast<double>(cur.c.y), cur.c.z + .5},
                                    terrainAt);
                            }
                            if (!runway ||
                                !automine::jumpClear({cur.c.x, cur.c.y, cur.c.z},
                                                     {land.x, land.y, land.z}, terrainAt))
                                continue;
                            const double ng =
                                cg + 2.0 + .7 * dist + 1.5 * runup + (pad ? placementCost : 0.);
                            if (!gScore.count(land) || ng < gScore[land] - 1e-9) {
                                gScore[land] = ng;
                                cameFrom[land] = cur.c;
                                digFlag[land] = false;
                                bridgeFlag[land] = pad;
                                parkourFlag[land] = true;
                                pillarFlag[land] = false;
                                placedCount[land] = used + (pad ? 1 : 0);
                                pillarRun[land] = 0;
                                open.push({ng + heuristic(land), land});
                            }
                        }
                }
            }
        }
        const bool progressed = !open.empty() && !(bestNode == segStart) && bestH < startH - 1.0;
        const CellKey segEnd = haveFound ? found : bestNode;
        if (!haveFound && !progressed) {
            if (cells.empty()) {
                note("path: no route (" + std::to_string(expansions) + " expansions)");
                discardPathIfExhausted();
                return false;
            }
            partial = true;
            break;
        }
        // Append this segment's chain (skipping the joint cell
        // when a previous segment already ended there).
        std::vector<CellKey> segCells;
        for (CellKey c = segEnd;; c = cameFrom[c]) {
            segCells.push_back(c);
            if (c == segStart)
                break;
        }
        std::reverse(segCells.begin(), segCells.end());
        for (std::size_t i = cells.empty() ? 0 : 1; i < segCells.size(); ++i) {
            cells.push_back(segCells[i]);
            if (digFlag.count(segCells[i]))
                allDig[segCells[i]] = digFlag[segCells[i]];
            if (bridgeFlag.count(segCells[i]))
                allBridge[segCells[i]] = bridgeFlag[segCells[i]];
            if (parkourFlag.count(segCells[i]))
                allParkour[segCells[i]] = parkourFlag[segCells[i]];
            if (pillarFlag.count(segCells[i]))
                allPillar[segCells[i]] = pillarFlag[segCells[i]];
        }
        segmentUsed = placedCount[segEnd];
        if (haveFound) {
            reachedGoal = true;
            break;
        }
        partial = true; // until a later segment reaches the goal
        segStart = bestNode;
    }
    partial = !reachedGoal;
    // Revalidate the final stance before committing any bridge/pillar route.
    // Partial segments are approaches only; they must never imply mining reach.
    if (reachedGoal && !cells.empty()) {
        const auto end = cells.back();
        const bool valid = pickup ? automine::inPickupReach(
                                       automine::pickupStancePoint({end.x, end.y, end.z}, item), item)
                           : workstation ? workstationReachableFrom(
                                               {end.x + .5f, float(end.y), end.z + .5f}, target_)
                                         : automine::inMiningReach(
                                               {end.x + .5, end.y + 1.62, end.z + .5}, targetCell);
        if (!valid) {
            note("path: final stance outside action reach; discarded");
            invalidateRoute();
            return false;
        }
    }
    path_.clear();
    pathDig_.clear();
    pathBridge_.clear();
    pathParkour_.clear();
    pathPillar_.clear();
    pathIdx_ = 0;
    // Retain every executable cell, including the approach to a dig.
    // Compression lost the stance needed for subsequent movement actions.
    // A cell-centre goal can be valid even when the player's actual position
    // in that same cell isn't. Walk the remaining sub-cell distance.
    const std::size_t first = walkOnly && reachedGoal && cells.size() == 1 ? 0 : 1;
    for (std::size_t i = first; i < cells.size(); ++i) {
        path_.push_back({cells[i].x, cells[i].y, cells[i].z});
        pathDig_.push_back(allDig.count(cells[i]) && allDig[cells[i]]);
        pathBridge_.push_back(allBridge.count(cells[i]) && allBridge[cells[i]]);
        pathParkour_.push_back(allParkour.count(cells[i]) && allParkour[cells[i]]);
        pathPillar_.push_back(allPillar.count(cells[i]) && allPillar[cells[i]]);
    }
    std::set<BlockPos3> requiredSupports;
    for (std::size_t i = 0; i < path_.size(); ++i)
        if (pathBridge_[i] || pathPillar_[i]) {
            const auto p = path_[i];
            const BlockPos3 floor{p.x, p.y - 1, p.z};
            const auto id = blockAt(floor);
            if (!isSolidCell(id) || !automine::fullCubeSupport(id))
                requiredSupports.insert(floor);
        }
    if (requiredSupports.size() > static_cast<std::size_t>(planSpares_)) {
        note("path: reconstructed route exceeds available blocks; discarded");
        invalidateRoute();
        return false;
    }
    segmentUsed = static_cast<int>(requiredSupports.size());
    pathFor_ = target_;
    pathGoal_ = target_;
    pathPartial_ = partial;
    note("path: " + std::to_string(path_.size()) + " waypoints (" + std::to_string(expansions) +
         " expansions, " + std::to_string(segments) + " segments, " + std::to_string(segmentUsed) +
         " blocks)" + (partial ? " partial" : ""));
    return true;
}

} // namespace automine::client
