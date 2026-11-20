#include "auto_mine.hpp"
#include "diagnostics.hpp"

namespace automine::client {

void AutoMineMod::clearPath() {
    std::lock_guard lock(pathMutex_);
    pathWalk_.clear();
    pathDigLine_.clear();
    pathBridgeLine_.clear();
    pathMarkers_.clear();
    pathBox_ = false;
}

automine::Terrain AutoMineMod::planTerrain(automine::PlanCell p) {
    const auto id = blockAt({p.x, p.y, p.z});
    if (id.empty())
        return automine::Terrain::Unknown;
    if (isFluidCell(id))
        return automine::Terrain::Hazard;
    if (localPart(id) == "bedrock")
        return automine::Terrain::Unbreakable;
    return isPassableCell(id) ? automine::Terrain::Air : automine::Terrain::Solid;
}

std::optional<BlockPos3> AutoMineMod::previewWorkCell() {
    if (phase_ == Phase::Idle)
        return {};
    if (attackHeld_ && phase_ != Phase::Combat && phase_ != Phase::Forage) {
        if (phase_ == Phase::Mine && hasTarget_)
            return target_;
        if (digPos_.x != INT_MIN)
            return digPos_;
        if (nativeMining_)
            return nativeMiningCell_;
    }
    if (phase_ == Phase::Smelt) {
        if (smeltPocketing_ && smeltPocket_.x != INT_MIN)
            return smeltPocket_;
        if (smeltPlaced_)
            return smeltFurnacePos_;
        if (smeltPlaceCell_.x != INT_MIN)
            return smeltPlaceCell_;
    }
    if (phase_ == Phase::Craft) {
        if (chainPocketing_ && chainPocket_.x != INT_MIN)
            return chainPocket_;
        if (chainPlaceCell_.x != INT_MIN)
            return chainPlaceCell_;
        if (chainTablePlaced_)
            return chainTablePos_;
    }
    if (hasTarget_ && (phase_ == Phase::Mine || phase_ == Phase::Navigate ||
                       phase_ == Phase::Collect || phase_ == Phase::NeedTool))
        return target_;
    return {};
}

automine::ActionPlan AutoMineMod::futureActions(const bedrock::Vec3 &feet) {
    auto plan = plannedActions(feet);
    if (attackHeld_ && phase_ != Phase::Combat && phase_ != Phase::Forage)
        if (const auto active = previewWorkCell(); active && isDiggableCell(blockAt(*active)))
            automine::prioritizeWork(plan, automine::Action::Dig,
                                     {active->x, active->y, active->z});
    return plan;
}

automine::ActionPlan AutoMineMod::plannedActions(const bedrock::Vec3 &feet) {
    using A = automine::Action;
    const automine::PlanCell here{static_cast<int>(std::floor(feet.x)),
                                  static_cast<int>(std::floor(feet.y)),
                                  static_cast<int>(std::floor(feet.z))};
    const auto read = [this](automine::PlanCell p) { return planTerrain(p); };
    if (phase_ == Phase::Descend)
        return automine::shaftPlan(here, descendTargetY_, read);
    if (phase_ == Phase::Hunt) {
        const automine::PlanCell base =
            huntStepActive_
                ? automine::PlanCell{huntStep_.x - huntDirX_, huntStep_.y, huntStep_.z - huntDirZ_}
                : here;
        return automine::tunnelPlan(base, huntDirX_, huntDirZ_, 6 - huntCells_, read);
    }
    automine::ActionPlan plan;
    if (phase_ == Phase::Ascend) {
        if (ascendShiftActive_) {
            for (const auto &p : automine::surfaceRoute(
                     here, {ascendShiftCell_.x, ascendShiftCell_.y, ascendShiftCell_.z}, read))
                plan.add(A::Walk, p);
            return plan;
        }
        const bool walkingToShaft = descendShaftCell_.x != INT_MIN &&
            here.y <= descendShaftCell_.y + 2 &&
            std::hypot(descendShaftCell_.x + .5 - feet.x,
                       descendShaftCell_.z + .5 - feet.z) > 1.2;
        if (!walkingToShaft) {
            if (ascendStairLatch_) {
                const automine::PlanCell next{here.x + ascendDirX_, here.y + 1,
                                              here.z + ascendDirZ_};
                for (int y : {next.y, next.y + 1})
                    if (read({next.x, y, next.z}) == automine::Terrain::Solid)
                        plan.add(A::Dig, {next.x, y, next.z});
                if (read({next.x, next.y - 1, next.z}) == automine::Terrain::Solid)
                    plan.add(A::Jump, next);
                return plan;
            }
            return automine::pillarReturnPlan(here, ascendTargetY_, read);
        }
        // The shaft approach uses the same retained path as normal navigation.
    }
    if (phase_ == Phase::Smelt || phase_ == Phase::Craft) {
        const bool smelt = phase_ == Phase::Smelt;
        const bool pocketing = smelt ? smeltPocketing_ : chainPocketing_;
        const auto pocket = smelt ? smeltPocket_ : chainPocket_;
        const auto place = smelt ? smeltPlaceCell_ : chainPlaceCell_;
        if (pocketing && pocket.x != INT_MIN) {
            if (isDiggableCell(blockAt(pocket)))
                plan.add(A::Dig, {pocket.x, pocket.y, pocket.z});
            plan.add(A::Place, {pocket.x, pocket.y, pocket.z});
        } else if (smelt && smeltStage_ == SmeltStage::BreakFurnace && smeltPlaced_) {
            const auto p = smeltFurnacePos_;
            plan.add(A::Dig, {p.x, p.y, p.z});
        } else if (smelt && smeltStage_ == SmeltStage::CollectDrops && smeltPlaced_) {
            const auto p = smeltFurnacePos_;
            for (const auto &cell : automine::surfaceRoute(here, {p.x, p.y, p.z}, read))
                plan.add(A::Collect, cell);
        } else if (place.x != INT_MIN && !(smelt ? smeltPlaced_ : chainTablePlaced_)) {
            plan.add(A::Place, {place.x, place.y, place.z});
        }
        return plan;
    }
    const bool routePhase = phase_ == Phase::Navigate || phase_ == Phase::Mine ||
                            phase_ == Phase::Collect || phase_ == Phase::NeedTool ||
                            phase_ == Phase::Recover || phase_ == Phase::Ascend;
    if (!routePhase)
        return plan;
    const bool returning = phase_ == Phase::Recover || phase_ == Phase::Ascend;
    const auto goal = returning ? target_ : !savedId_.empty() ? savedTarget_ : target_;
    if (phase_ == Phase::Collect) {
        // r18: collection is routed by the same normal A* path as mining targets.
        // Entity target positions are not a custom sweep or ring.
        if (collectEntityValid_) {
            plan.complete = !pathPartial_;
            plan.add(A::Collect, {target_.x, target_.y, target_.z});
        }
        return plan;
    }
    if (!path_.empty() && pathGoal_ == goal) {
        automine::PlanCell previous = here;
        std::set<BlockPos3> previewSupports;
        for (std::size_t i = pathIdx_; i < path_.size(); ++i) {
            const auto p = path_[i];
            if (p.y > previous.y && !pathPillar_[i]) {
                const automine::PlanCell cap{previous.x, previous.y + 2, previous.z};
                if (read(cap) == automine::Terrain::Solid)
                    plan.add(A::Dig, cap);
            }
            // Preserve execution order: cap/head, feet, support, movement.
            for (int y : {p.y + 1, p.y}) {
                const automine::PlanCell cell{p.x, y, p.z};
                if (read(cell) == automine::Terrain::Solid)
                    plan.add(A::Dig, cell);
            }
            if (pathParkour_[i] &&
                !(i == pathIdx_ && parkourStage_ >= 1 && parkourWaypoint_ == p)) {
                const double dx = p.x - previous.x, dz = p.z - previous.z, len = std::hypot(dx, dz);
                const int runup = automine::jumpRunup(previous, {p.x, p.y, p.z});
                if (runup && len > 0) {
                    plan.add(A::Walk,
                             {previous.x - static_cast<int>(std::round(dx / len)) * runup,
                              previous.y,
                              previous.z - static_cast<int>(std::round(dz / len)) * runup});
                    plan.add(A::Walk, previous);
                }
            }
            if (pathPillar_[i]) {
                const automine::PlanCell cap{p.x, p.y + 2, p.z};
                if (read(cap) == automine::Terrain::Solid)
                    plan.add(A::Dig, cap);
                plan.add(A::Jump, {p.x, p.y, p.z});
                if (!isSolidCell(blockAt({p.x, p.y - 1, p.z})))
                    plan.add(A::Place, {p.x, p.y - 1, p.z});
                previous = {p.x, p.y, p.z};
                continue;
            }
            if (pathBridge_[i] && !pathParkour_[i]) {
                std::size_t end = i;
                while (end + 1 < path_.size() && pathBridge_[end + 1] && !pathParkour_[end + 1] &&
                       !pathPillar_[end + 1] && path_[end + 1].y == p.y)
                    ++end;
                for (std::size_t j = end + 1; j-- > i;) {
                    const auto step = path_[j];
                    const BlockPos3 floor{step.x, step.y - 1, step.z};
                    if (!isSolidCell(blockAt(floor)) && previewSupports.insert(floor).second)
                        plan.add(A::Place, {floor.x, floor.y, floor.z});
                }
            } else if (pathBridge_[i] && !isSolidCell(blockAt({p.x, p.y - 1, p.z})))
                plan.add(A::Place, {p.x, p.y - 1, p.z});
            plan.add((pathParkour_[i] || p.y > previous.y) ? A::Jump : A::Walk, {p.x, p.y, p.z});
            previous = {p.x, p.y, p.z};
        }
        plan.complete = !pathPartial_;
    }
    if (returning) {
        if (plan.actions.empty())
            for (const auto &p : automine::surfaceRoute(here, {goal.x, goal.y, goal.z}, read))
                plan.add(A::Walk, p);
        return plan; // the destination is a position, never a block to mine
    }
    if (hasTarget_) {
        // The active occluder is a branch from the retained main route.
        if (!savedId_.empty())
            plan.actions.insert(plan.actions.begin(), {A::Dig, {target_.x, target_.y, target_.z}});
        if (phase_ == Phase::Collect) {
            if (collectEntityValid_) {
                plan.add(A::Collect, {target_.x, target_.y, target_.z});
            }
        } else if (plan.complete &&
                   (!path_.empty() || automine::inMiningReach({feet.x, feet.y + 1.62, feet.z},
                                                              {goal.x, goal.y, goal.z}))) {
            bedrock::Vec3 stance = feet;
            if (!path_.empty() && pathGoal_ == goal) {
                const auto p = path_.back();
                stance = {p.x + .5f, static_cast<float>(p.y), p.z + .5f};
            }
            const bedrock::Vec3 from{stance.x, stance.y + 1.62f, stance.z};
            const double dx = goal.x + .5 - from.x, dy = goal.y + .5 - from.y,
                         dz = goal.z + .5 - from.z;
            const int steps =
                std::max(1, static_cast<int>(std::ceil(std::hypot(dx, dy, dz) / .12)));
            std::set<BlockPos3> seen;
            for (const auto &action : plan.actions)
                if (action.kind == A::Dig)
                    seen.insert({action.cell.x, action.cell.y, action.cell.z});
            for (int i = 1; i <= steps; ++i) {
                const double t = static_cast<double>(i) / steps;
                const BlockPos3 p{static_cast<int>(std::floor(from.x + dx * t)),
                                  static_cast<int>(std::floor(from.y + dy * t)),
                                  static_cast<int>(std::floor(from.z + dz * t))};
                if (seen.insert(p).second && isDiggableCell(blockAt(p)))
                    plan.add(A::Dig, {p.x, p.y, p.z});
            }
        }
    }
    return plan;
}

void AutoMineMod::updatePath(const bedrock::Vec3 &feet) {
    using A = automine::Action;
    std::vector<bedrock::Vec3> walk, dig, bridge, markers;
    bool box = false;
    BlockPos3 boxPos{};
    if (showPath_) {
        const auto plan = futureActions(feet);
        bedrock::Vec3 stance{feet.x, feet.y + .08f, feet.z};
        auto edge = [](auto &dst, bedrock::Vec3 from, bedrock::Vec3 to) {
            const int n =
                std::max(1, static_cast<int>(std::ceil(
                                std::hypot(to.x - from.x, to.y - from.y, to.z - from.z) / .2)));
            for (int i = 0; i < n; ++i) {
                const float a = static_cast<float>(i) / n, b = static_cast<float>(i + 1) / n;
                dst.push_back({from.x + (to.x - from.x) * a, from.y + (to.y - from.y) * a,
                               from.z + (to.z - from.z) * a});
                dst.push_back({from.x + (to.x - from.x) * b, from.y + (to.y - from.y) * b,
                               from.z + (to.z - from.z) * b});
            }
        };
        automine::TerrainCache worldTerrain(
            [this](automine::PlanCell p) { return planTerrain(p); });
        std::map<std::tuple<int, int, int>, automine::Terrain> forecast;
        const auto terrain = [&](automine::PlanCell p) {
            const auto it = forecast.find({p.x, p.y, p.z});
            return it == forecast.end() ? worldTerrain(p) : it->second;
        };
        for (const auto &action : plan.actions) {
            const auto p = action.cell;
            const bedrock::Vec3 point{p.x + .5f, p.y + .08f, p.z + .5f};
            if (action.kind == A::Dig || action.kind == A::Place) {
                auto &dst = action.kind == A::Dig ? dig : bridge;
                const bedrock::Vec3 hit{point.x, p.y + .5f, point.z};
                edge(dst, stance, hit);
                // Every future work cell has a marker, even when the
                // shaft line lies behind the block currently being mined.
                constexpr float r = .43f;
                edge(markers, {hit.x - r, hit.y, hit.z - r}, {hit.x + r, hit.y, hit.z - r});
                edge(markers, {hit.x + r, hit.y, hit.z - r}, {hit.x + r, hit.y, hit.z + r});
                edge(markers, {hit.x + r, hit.y, hit.z + r}, {hit.x - r, hit.y, hit.z + r});
                edge(markers, {hit.x - r, hit.y, hit.z + r}, {hit.x - r, hit.y, hit.z - r});
                auto &work = action.kind == A::Place ? bridge : markers;
                for (float x : {p.x + .04f, p.x + .96f})
                    for (float z : {p.z + .04f, p.z + .96f})
                        edge(work, {x, p.y + .04f, z}, {x, p.y + .96f, z});
                forecast[{p.x, p.y, p.z}] =
                    action.kind == A::Dig ? automine::Terrain::Air : automine::Terrain::Solid;

            } else {
                if (action.kind == A::Jump) {
                    automine::PlanCell launch{static_cast<int>(std::floor(stance.x)),
                                              static_cast<int>(std::floor(stance.y)),
                                              static_cast<int>(std::floor(stance.z))};
                    const bool activeJump = pathIdx_ < path_.size() && pathParkour_[pathIdx_] &&
                                            path_[pathIdx_] == BlockPos3{p.x, p.y, p.z} &&
                                            parkourStage_ >= 1;
                    if (activeJump)
                        launch = {parkourFrom_.x, parkourFrom_.y, parkourFrom_.z};
                    const auto arc = automine::jumpTrajectory(launch, p);
                    auto last = stance;
                    for (const auto &q : arc) {
                        if (activeJump && ((q.x - stance.x) * (p.x - launch.x) +
                                           (q.z - stance.z) * (p.z - launch.z)) < -.05)
                            continue;
                        const bedrock::Vec3 next{static_cast<float>(q.x),
                                                 static_cast<float>(q.y + .08),
                                                 static_cast<float>(q.z)};
                        if (!automine::bodyClear(q, terrain))
                            break;
                        edge(walk, last, next);
                        last = next;
                    }
                } else {
                    const automine::NavPoint from{stance.x, stance.y - .08, stance.z};
                    const automine::NavPoint to{point.x, point.y - .08, point.z};
                    const auto samples = point.y < stance.y - .4f
                                             ? automine::dropTrajectory(from, to)
                                             : automine::groundTrajectory(from, to, terrain);
                    auto last = stance;
                    for (const auto &q : samples) {
                        if (!automine::bodyClear(q, terrain))
                            break;
                        const bedrock::Vec3 next{static_cast<float>(q.x),
                                                 static_cast<float>(q.y + .08),
                                                 static_cast<float>(q.z)};
                        edge(walk, last, next);
                        last = next;
                    }
                }
                constexpr float footprint = .25f;
                edge(walk, {point.x - footprint, point.y, point.z - footprint},
                     {point.x + footprint, point.y, point.z - footprint});
                edge(walk, {point.x + footprint, point.y, point.z - footprint},
                     {point.x + footprint, point.y, point.z + footprint});
                edge(walk, {point.x + footprint, point.y, point.z + footprint},
                     {point.x - footprint, point.y, point.z + footprint});
                edge(walk, {point.x - footprint, point.y, point.z + footprint},
                     {point.x - footprint, point.y, point.z - footprint});
                // Direction chevrons make the future action order visible.
                const double dx = point.x - stance.x, dy = point.y - stance.y,
                             dz = point.z - stance.z;
                const double len = std::hypot(dx, dy, dz);
                if (len > .2) {
                    const bedrock::Vec3 base{point.x - static_cast<float>(dx / len) * .22f,
                                             point.y - static_cast<float>(dy / len) * .22f,
                                             point.z - static_cast<float>(dz / len) * .22f};
                    const bedrock::Vec3 side =
                        std::abs(dy / len) > .9
                            ? bedrock::Vec3{.1f, 0, 0}
                            : bedrock::Vec3{static_cast<float>(dz / len) * .1f, 0,
                                            -static_cast<float>(dx / len) * .1f};
                    edge(walk, point, {base.x + side.x, base.y + side.y, base.z + side.z});
                    edge(walk, point, {base.x - side.x, base.y - side.y, base.z - side.z});
                }
                stance = point;
            }
        }
        if (const auto work = previewWorkCell()) {
            box = true;
            boxPos = *work;
        }
    }
    std::lock_guard lock(pathMutex_);
    pathWalk_ = std::move(walk);
    pathDigLine_ = std::move(dig);
    pathBridgeLine_ = std::move(bridge);
    pathMarkers_ = std::move(markers);
    pathBox_ = box;
    pathBoxPos_ = boxPos;
    pathBreakProgress_ = box && destroyTarget_.x == boxPos.x && destroyTarget_.y == boxPos.y &&
                                 destroyTarget_.z == boxPos.z
                             ? std::clamp(destroyProgress_, 0.f, 1.f)
                             : -1.f;
}

void BDS_HOOK_CALL AutoMineMod::onRenderLevel(void *user, void *raw, BDS_HookNext next,
                                              void *frame) {
    auto &self = *static_cast<AutoMineMod *>(user);
    next(frame, raw);
    self.drawPathGuarded(raw);
}

void AutoMineMod::drawPath(void *raw) {
    crashrec::Section section("drawPath");
    if (!outlineReady_ || !showPath_)
        return;
    const auto &args = *static_cast<BDS_RenderLevelArgs *>(raw);
    if (!args.renderer || !args.screen)
        return;
    std::vector<bedrock::Vec3> walk, dig, bridge, markers;
    bool box{};
    BlockPos3 boxPos{};
    float breakProgress{};
    {
        std::lock_guard lock(pathMutex_);
        walk = pathWalk_;
        dig = pathDigLine_;
        bridge = pathBridgeLine_;
        markers = pathMarkers_;
        box = pathBox_;
        boxPos = pathBoxPos_;
        breakProgress = pathBreakProgress_;
    }
    if (walk.size() < 2 && dig.size() < 2 && bridge.size() < 2 && !box)
        return;
    const auto origin = outline_.renderOrigin(args.renderer);
    if (!origin)
        return;
    auto rel = [&](const bedrock::Vec3 &p) {
        return bedrock::Vec3{p.x - origin->x, p.y - origin->y, p.z - origin->z};
    };
    bedrock::WorldOutlineGeometry geometry;
    geometry.origin = *origin;
    auto addLine = [&](const std::vector<bedrock::Vec3> &pts, const std::array<float, 4> &color) {
        if (pts.size() < 2)
            return;
        bedrock::WorldGeometryBatch batch;
        batch.color = color;
        for (std::size_t i = 0; i + 1 < pts.size(); i += 2) {
            batch.vertices.push_back(rel(pts[i]));
            batch.vertices.push_back(rel(pts[i + 1]));
        }
        geometry.lines.push_back(std::move(batch));
    };
    addLine(walk, {0.3f, 1.f, 0.35f, 1.f});  // route: bright green
    addLine(dig, {1.f, 0.8f, 0.15f, 1.f});   // dig-through: yellow
    addLine(bridge, {0.3f, 0.9f, 1.f, 1.f}); // bridge cells: cyan
    addLine(markers, {1.f, .55f, .2f, 1.f});
    if (box) {
        bedrock::WorldGeometryBatch batch;
        batch.color = {1.f, 0.45f, 0.1f, 1.f};
        const float x0 = static_cast<float>(boxPos.x), y0 = static_cast<float>(boxPos.y),
                    z0 = static_cast<float>(boxPos.z);
        const float x1 = x0 + 1.f, y1 = y0 + 1.f, z1 = z0 + 1.f;
        const bedrock::Vec3 c[8]{{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1},
                                 {x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1}};
        constexpr int edges[12][2]{{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                   {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto &e : edges) {
            batch.vertices.push_back(rel(c[e[0]]));
            batch.vertices.push_back(rel(c[e[1]]));
        }
        if (breakProgress >= 0.f) {
            const float p = std::clamp(breakProgress, 0.f, 1.f);
            const float half = 0.04f + p * .46f;
            const float cx = x0 + .5f, cy = y0 + .5f, cz = z0 + .5f;
            const bedrock::Vec3 inner[8]{
                {cx - half, cy - half, cz - half}, {cx + half, cy - half, cz - half},
                {cx + half, cy - half, cz + half}, {cx - half, cy - half, cz + half},
                {cx - half, cy + half, cz - half}, {cx + half, cy + half, cz - half},
                {cx + half, cy + half, cz + half}, {cx - half, cy + half, cz + half}};
            bedrock::WorldGeometryBatch innerBatch;
            innerBatch.color = {1.f, .78f, .2f, 1.f};
            for (const auto &e : edges) {
                innerBatch.vertices.push_back(rel(inner[e[0]]));
                innerBatch.vertices.push_back(rel(inner[e[1]]));
            }
            geometry.lines.push_back(std::move(innerBatch));
        }
        geometry.lines.push_back(std::move(batch));
    }
    outline_.draw(args.renderer, args.screen, geometry);
}

} // namespace automine::client
