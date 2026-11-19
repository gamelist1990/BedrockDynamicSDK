#include "auto_mine.hpp"

namespace automine::client {

void AutoMineMod::tickFollowPath(bedrock::Vec3 feet) {
    // A route interruption (including water/timeout) cannot inherit jump ownership.
    if (parkourSprintInput_ &&
        (pathIdx_ >= path_.size() || pathIdx_ >= pathParkour_.size() ||
         !pathParkour_[pathIdx_] || inWaterNow(feet))) {
        parkourSprintInput_ = false;
        pressKey(kKeySprint, false);
        pressKey(kKeySpace, false);
    }
    ++navTicks_;
    const std::size_t idxAtEntry = pathIdx_;
    if (navTicks_ > 20 * 45) { // same overall budget as legacy navigation
        note("navigate timeout (path); blacklist target");
        if (chainWaitingCobble_)
            ++cobbleNavFails_;
        blacklistCluster(target_, targetId_);
        hasTarget_ = false;
        pressKey(kKeyW, false);
        pressKey(kKeySpace, false);
        pressKey(kKeySprint, false);
        parkourSprintInput_ = false;
        path_.clear();
        pathDig_.clear();
        pathBridge_.clear();
        pathParkour_.clear();
        pathPillar_.clear();
        phase_ = Phase::Scan;
        scanCursor_ = 0;
        bestDist2_ = 1e18;
        return;
    }
    if (pathIdx_ >= path_.size()) {
        pressKey(kKeyW, false);
        pressKey(kKeySpace, false);
        if (pathPartial_) {
            pathPartial_ = false;
            pathFor_ = {INT_MIN, 0, 0};
            navTicks_ = 0;
            return;
        }
        if (++replans_ > 2) {
            note("path exhausted; blacklist target");
            blacklist_.insert(target_);
            hasTarget_ = false;
            phase_ = Phase::Scan;
            scanCursor_ = 0;
            bestDist2_ = 1e18;
        } else
            pathFor_ = {INT_MIN, 0, 0}; // force a re-plan next tick
        return;
    }
    if (inWaterNow(feet)) {
        const BlockPos3 wp0 = path_[pathIdx_];
        const bedrock::Vec3 eye0{feet.x, feet.y + 1.62f, feet.z};
        faceAim(eye0, {wp0.x + .5f, eye0.y, wp0.z + .5f}, 10.f, 0.25f);
        if (headInWater(feet)) {
            pressKey(kKeyW, false);
            pressKey(kKeySpace, true);
            const auto above = blockAt({static_cast<int>(std::floor(feet.x)),
                                        static_cast<int>(std::floor(feet.y + 2.62f)),
                                        static_cast<int>(std::floor(feet.z))});
            const bool ceiling =
                !above.empty() && !isAir(above) && !contains(localPart(above), "water");
            pressKey(kKeyS, ceiling);
        } else {
            pressKey(kKeyS, false);
            pressKey(kKeyW, true);
            pressKey(kKeySpace, true);
            if (std::hypot(wp0.x + .5 - feet.x, wp0.z + .5 - feet.z) < 1.2)
                ++pathIdx_;
        }
        return;
    }
    if (pathBridge_[pathIdx_] && !pathParkour_[pathIdx_]) {
        if (tickBridge(feet)) {
            navTicks_ = 0;
            return;
        } // bridging is progress
        // Floor filled: fall through to normal following this tick.
    }
    if (pathIdx_ < pathPillar_.size() && pathPillar_[pathIdx_]) {
        tickPillarWaypoint(feet);
        return;
    }
    if (pathIdx_ < pathParkour_.size() && pathParkour_[pathIdx_]) {
        if (tickParkourWaypoint(feet))
            return;
        if (pathIdx_ >= path_.size())
            return;
    }
    const BlockPos3 wp = path_[pathIdx_];
    const int stanceY = static_cast<int>(std::floor(feet.y));
    if (path_[pathIdx_].y > stanceY && feet.y - stanceY < .05f) {
        const BlockPos3 cap{static_cast<int>(std::floor(feet.x)), stanceY + 2,
                            static_cast<int>(std::floor(feet.z))};
        const auto id = blockAt(cap);
        if (isDiggableCell(id)) {
            pressKey(kKeyW, false);
            pressKey(kKeySpace, false);
            enterPathDigCell(cap, id);
            return;
        }
    }
    // Dig only in reach; clearing a waypoint does not consume its walk.
    if (!pathBridge_[pathIdx_]) {
        const auto wf = blockAt(wp);
        const auto wh = blockAt({wp.x, wp.y + 1, wp.z});
        const bool fSolid = isSolidCell(wf), hSolid = isSolidCell(wh);
        if (fSolid || hSolid) {
            const BlockPos3 dig{wp.x, hSolid ? wp.y + 1 : wp.y, wp.z};
            const auto &id = hSolid ? wh : wf;
            if (isDiggableCell(id)) {
                pathDig_[pathIdx_] = true;
                if (std::hypot(dig.x + .5 - feet.x, dig.y + .5 - feet.y - 1.62,
                               dig.z + .5 - feet.z) <= 4.0) {
                    pressKey(kKeyW, false);
                    pressKey(kKeySpace, false);
                    enterPathDigCell(dig, id);
                    return;
                }
            } else {
                pressKey(kKeyW, false);
                planBlockedFor_ = target_;
                if (planBlocked_.size() < 64)
                    planBlocked_.insert({wp.x, wp.y, wp.z});
                pathFor_ = {INT_MIN, 0, 0};
                note("path: waypoint blocked, replanning");
                return;
            }
        } else if (isPassableCell(wf) && isPassableCell(wh)) {
            pathDig_[pathIdx_] = false;
        }
    }
    const int feetCellY = static_cast<int>(std::floor(feet.y));
    float yawNow{}, pitchNow{};
    const bool haveYaw = !independent_ && rotation(yawNow, pitchNow);
    const auto isCorner = [&](std::size_t i) {
        if (i + 1 >= path_.size())
            return false;
        const BlockPos3 previous = i ? path_[i - 1]
                                     : BlockPos3{static_cast<int>(std::floor(feet.x)), feetCellY,
                                                 static_cast<int>(std::floor(feet.z))};
        const auto a = path_[i], b = path_[i + 1];
        return a.y != previous.y || b.y != a.y || a.x - previous.x != b.x - a.x ||
               a.z - previous.z != b.z - a.z;
    };
    while (pathIdx_ < path_.size() && !pathDig_[pathIdx_] && !pathBridge_[pathIdx_] &&
           !pathParkour_[pathIdx_] && !pathPillar_[pathIdx_]) {
        const BlockPos3 w = path_[pathIdx_];
        const double hx = w.x + .5 - feet.x, hz = w.z + .5 - feet.z;
        const double horiz = std::hypot(hx, hz);
        bool passed = false;
        const bool corner = isCorner(pathIdx_);
        if (std::abs(feet.y - w.y) < .16)
            passed = horiz < (corner ? .30 : .48);
        else if (w.y < feetCellY)
            passed = feet.y <= w.y + .2 && horiz < .5;
        if (!passed && !corner && pathIdx_ > 0 && std::abs(feet.y - w.y) < .16) {
            const auto previous = path_[pathIdx_ - 1];
            const double vx = w.x - previous.x, vz = w.z - previous.z;
            const double length = std::hypot(vx, vz);
            if (length > .01) {
                const double along = ((feet.x - w.x - .5) * vx + (feet.z - w.z - .5) * vz) / length;
                const double lateral =
                    std::abs((feet.x - w.x - .5) * vz - (feet.z - w.z - .5) * vx) / length;
                passed = along >= 0 && along < .6 && lateral < .25;
            }
        }
        if (!passed)
            break;
        ++pathIdx_;
        stuckTicks_ = 0;
    }
    if (pathIdx_ != idxAtEntry)
        navTicks_ = 0;
    if (pathIdx_ >= path_.size()) {
        pressKey(kKeyW, false);
        pressKey(kKeySpace, false);
        pressKey(kKeyA, false);
        pressKey(kKeyD, false);
        return; // the exhausted-path branch at the top replans next tick
    }
    // Aim at the first waypoint at least ~1.7 blocks out (lookahead),
    // or at the current one when it is a Dig cell / the last one.
    automine::TerrainCache terrainCache([this](automine::PlanCell p) { return planTerrain(p); });
    const auto terrain = [&](automine::PlanCell p) { return terrainCache(p); };
    std::size_t aimIdx = pathIdx_;
    if (!pathDig_[pathIdx_] && !pathBridge_[pathIdx_]) {
        for (std::size_t i = pathIdx_; i < path_.size(); ++i) {
            if (pathDig_[i] || pathBridge_[i] || pathParkour_[i] || pathPillar_[i] ||
                path_[i].y != feetCellY)
                break;
            if (!automine::walkClear(
                    {feet.x, static_cast<double>(feetCellY), feet.z},
                    {path_[i].x + .5, static_cast<double>(path_[i].y), path_[i].z + .5}, terrain))
                break;
            if (isCorner(i)) {
                aimIdx = i;
                break;
            }
            aimIdx = i;
            if (std::hypot(path_[i].x + .5 - feet.x, path_[i].z + .5 - feet.z) >= 1.7)
                break;
        }
    }
    const bedrock::Vec3 eye{feet.x, feet.y + 1.62f, feet.z};
    const bedrock::Vec3 aim{path_[aimIdx].x + .5f, eye.y, path_[aimIdx].z + .5f};
    faceAim(eye, aim, 10.f, 0.25f);
    // Sharp turns pivot in place instead of arcing wide.
    bool wDown = true;
    if (haveYaw) {
        const double want =
            std::atan2(-(aim.x - feet.x), aim.z - feet.z) * 180.0 / 3.141592653589793;
        const double err = std::fmod(want - yawNow + 540.0, 360.0) - 180.0;
        if (std::abs(err) > 65.0)
            wDown = false;
    }
    pressKey(kKeyW, wDown);
    {
        bool wantSprint = false;
        if (wDown) {
            const BlockPos3 &last = path_.back();
            const double aimDist = std::hypot(last.x + .5 - feet.x, last.z + .5 - feet.z);
            bool levelAhead = true;
            int levelRun = 0;
            for (std::size_t i = pathIdx_; i < path_.size() && i < pathIdx_ + 4; ++i) {
                if (pathDig_[i] || pathBridge_[i] || pathParkour_[i] || pathPillar_[i] ||
                    isCorner(i) || path_[i].y != feetCellY) {
                    levelAhead = false;
                    break;
                }
                ++levelRun;
            }
            wantSprint = aimDist >= 3.0 && levelAhead && levelRun >= 2;
        }
        if (wantSprint && !sprintWasOn_)
            note("sprint");
        sprintWasOn_ = wantSprint;
        pressKey(kKeySprint, wantSprint);
    }
    bool jump = false;
    for (std::size_t i = pathIdx_; i < path_.size() && i < pathIdx_ + 3; ++i) {
        if (path_[i].y < feetCellY)
            break;
        if (path_[i].y > feetCellY) {
            jump = std::hypot(path_[i].x + .5 - feet.x, path_[i].z + .5 - feet.z) < 1.7;
            break;
        }
    }
    pressKey(kKeySpace, jump);
    if (jump) {
        if (jumpHoldTicks_ == 0)
            jumpBaseY_ = feetCellY;
        if (++jumpHoldTicks_ > 50 && feetCellY <= jumpBaseY_) {
            jumpHoldTicks_ = 0;
            pressKey(kKeySpace, false);
            pressKey(kKeyW, false);
            planBlockedFor_ = target_;
            if (planBlocked_.size() < 64)
                planBlocked_.insert({wp.x, wp.y, wp.z});
            pathFor_ = {INT_MIN, 0, 0};
            note("path: jump watchdog, replanning");
            return;
        }
    } else
        jumpHoldTicks_ = 0;
    if (navTicks_ % 10 == 0) {
        const double moved = std::hypot(feet.x - lastX_, feet.z - lastZ_);
        if (moved < 0.12) {
            if (++stuckTicks_ >= 3) {
                stuckTicks_ = 0;
                pressKey(kKeyW, false);
                pressKey(kKeySpace, false);
                pressKey(kKeyA, false);
                pressKey(kKeyD, false);
                if (++replans_ > 2) {
                    note("path follow stuck; blacklist target");
                    blacklist_.insert(target_);
                    hasTarget_ = false;
                    path_.clear();
                    pathDig_.clear();
                    pathBridge_.clear();
                    pathParkour_.clear();
                    pathPillar_.clear();
                    phase_ = Phase::Scan;
                    scanCursor_ = 0;
                    bestDist2_ = 1e18;
                } else {
                    if (pathIdx_ < path_.size() && planBlocked_.size() < 64) {
                        const BlockPos3 w = path_[pathIdx_];
                        planBlockedFor_ = target_;
                        planBlocked_.insert({w.x, w.y, w.z});
                        note("path: replan around blocked cell " + std::to_string(w.x) + "," +
                             std::to_string(w.y) + "," + std::to_string(w.z));
                    }
                    pathFor_ = {INT_MIN, 0, 0};
                }
                return;
            }
        } else
            stuckTicks_ = 0;
        lastX_ = feet.x;
        lastZ_ = feet.z;
    }
    if (stuckTicks_ == 1 || stuckTicks_ == 2) {
        pressKey(kKeySpace, true);
        pressKey(kKeyD, stuckTicks_ == 1);
        pressKey(kKeyA, stuckTicks_ == 2);
    } else {
        pressKey(kKeyA, false);
        pressKey(kKeyD, false);
    }
}

void AutoMineMod::tickNavigate(bedrock::Vec3 feet) {
    if (!hasTarget_) {
        phase_ = Phase::Scan;
        scanCursor_ = 0;
        bestDist2_ = 1e18;
        return;
    }
    // Target may have changed (another player mined it, physics, ...).
    const auto targetNow = blockAt(target_);
    if (navReturn_ && !(target_ == chainTablePos_))
        navReturn_ = false;
    if (navReturn_) {
        if (!contains(localPart(targetNow), "crafting_table")) {
            note("craft chain: table gone; abandoning return");
            navReturn_ = false;
            chainTablePlaced_ = false;
            hasTarget_ = false;
            phase_ = Phase::Scan;
            scanCursor_ = 0;
            bestDist2_ = 1e18;
            releaseInputs();
            return;
        }
    } else if (targetNow.empty() || localPart(targetNow) != targetId_) {
        note("target vanished; rescan");
        hasTarget_ = false;
        phase_ = Phase::Scan;
        scanCursor_ = 0;
        bestDist2_ = 1e18;
        releaseInputs();
        return;
    }
    const bedrock::Vec3 eye{feet.x, feet.y + 1.62f, feet.z};
    const bedrock::Vec3 center{target_.x + .5f, target_.y + .5f, target_.z + .5f};
    const double dx = center.x - eye.x, dy = center.y - eye.y, dz = center.z - eye.z;
    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const bool rejectedStance = mineRejectedFor_ == target_ &&
        mineRejectedStances_.count({static_cast<int>(std::floor(feet.x)),
                                    static_cast<int>(std::floor(feet.y)),
                                    static_cast<int>(std::floor(feet.z))});
    // Jump height can put the ore in reach before the last pillar block
    // exists. Finish that action (including its landing check) first.
    const auto constructionPending = [&] {
        return scaffoldActive_ || (pathFor_ == target_ && pathIdx_ < path_.size() &&
                                   (pathPillar_[pathIdx_] || pathBridge_[pathIdx_] ||
                                    pathParkour_[pathIdx_]));
    };
    // A placement action owns the selected slot until it is finished.
    if (!constructionPending() && !navReturn_ && !ensureTool(targetId_)) {
        enterNeedTool();
        return;
    }
    const auto supported = [&] {
        return navReturn_ || center.y <= eye.y || inWaterNow(feet) ||
               automine::supportFraction({feet.x, feet.y, feet.z},
                                        [this](automine::PlanCell c) { return planTerrain(c); }) > .5;
    };
    if (automine::miningHandoffReady({eye.x, eye.y, eye.z}, {target_.x, target_.y, target_.z},
                                    constructionPending(), rejectedStance, supported)) {
        for (auto key : {kKeyW, kKeyA, kKeyS, kKeyD, kKeySpace, kKeySprint, kKeySneak})
            pressKey(key, false);
        setUse(false);
        if (navReturn_) {
            navReturn_ = false;
            hasTarget_ = false;
            chainWaitingCobble_ = false;
            phase_ = Phase::Craft;
            chainStepTicks_ = 0;
            chainAimedTick_ = -1;
            note("craft chain: back at the table, resuming");
            return;
        }
        phase_ = Phase::Mine;
        mineTicks_ = 0;
        return;
    }
    if (!scaffoldActive_ && !(pathFor_ == target_)) {
        if (!(planFailFor_ == target_) || tickCount_ - planFailTick_ >= 200) {
            if (!planPath(feet)) {
                planFailFor_ = target_;
                planFailTick_ = tickCount_;
            }
        }
    }
    if (!constructionPending() && !path_.empty() && navTicks_ > 300 &&
        target_.y - static_cast<int>(std::floor(feet.y)) >= 3 && scaffoldStanceOk(feet)) {
        note("navigate: route stuck toward elevated target; switching to scaffold");
        engageScaffold(feet);
    }
    if (scaffoldActive_ || (!constructionPending() && scaffoldWanted(feet))) {
        if (tickScaffold(feet))
            return;
    }
    if (!path_.empty()) {
        tickFollowPath(feet);
        return;
    }
    if (!independent_ && !navReturn_ && !inWaterNow(feet) && boatSlot() < 0 && dist > 10.0 &&
        !(boatChainTriedFor_ == target_) && routeCrossesWater(feet, center) &&
        tryStartBoatChain()) {
        tickCraft(feet);
        return;
    }
    if (inWaterNow(feet)) {
        if (!wasInWater_)
            note("swimming to target");
        wasInWater_ = true;
        // A long crossing with a boat in the hotbar: ride instead.
        if (!independent_ && boatSlot() >= 0 && dist > 10.0 && !(boatTriedFor_ == target_)) {
            enterBoat();
            tickBoat(feet);
            return;
        }
        faceAim(eye, {center.x, eye.y, center.z}, 10.f, 0.25f);
        if (headInWater(feet)) {
            ++swimSubmerged_;
            pressKey(kKeyW, false);
            pressKey(kKeySpace, true);
            const auto above = blockAt({static_cast<int>(std::floor(feet.x)),
                                        static_cast<int>(std::floor(feet.y + 2.62f)),
                                        static_cast<int>(std::floor(feet.z))});
            const bool ceiling =
                !above.empty() && !isAir(above) && !contains(localPart(above), "water");
            pressKey(kKeyS, ceiling);
            pressKey(kKeyD, ceiling && swimSubmerged_ > 20 * 6);
        } else {
            swimSubmerged_ = 0;
            pressKey(kKeyS, false);
            pressKey(kKeyD, false);
            pressKey(kKeyW, true);
            pressKey(kKeySpace, true);
        }
        if (swimSubmerged_ > 20 * 10) {
            swimSubmerged_ = 0;
            wasInWater_ = false;
            pressKey(kKeyS, false);
            pressKey(kKeyD, false);
            startEscape();
            tickEscape(feet);
            return;
        }
        ++navTicks_;
        if (navTicks_ % 10 == 0) {
            const double moved = std::hypot(feet.x - lastX_, feet.z - lastZ_);
            if (moved < 0.12) {
                if (++stuckTicks_ >= 10) {
                    note("swim stuck; blacklist target");
                    blacklist_.insert(target_);
                    hasTarget_ = false;
                    pressKey(kKeyW, false);
                    pressKey(kKeySpace, false);
                    phase_ = Phase::Scan;
                    scanCursor_ = 0;
                    bestDist2_ = 1e18;
                    return;
                }
            } else
                stuckTicks_ = 0;
            lastX_ = feet.x;
            lastZ_ = feet.z;
        }
        if (navTicks_ > 20 * 45) {
            note("navigate timeout (swim); blacklist target");
            if (chainWaitingCobble_)
                ++cobbleNavFails_;
            blacklist_.insert(target_);
            hasTarget_ = false;
            pressKey(kKeyW, false);
            pressKey(kKeySpace, false);
            phase_ = Phase::Scan;
            scanCursor_ = 0;
            bestDist2_ = 1e18;
        }
        return;
    }
    if (wasInWater_) { // reached land: re-plan from the shore
        wasInWater_ = false;
        swimSubmerged_ = 0;
        surfacing_ = false;
        pressKey(kKeyS, false);
        pressKey(kKeyD, false);
        pathFor_ = {INT_MIN, 0, 0};
    }
    faceAim(eye, center, 10.f, 0.25f);
    // Cliff guard: do not step into a >2 block drop straight ahead.
    const double hx = center.x - feet.x, hz = center.z - feet.z;
    const double hl = std::hypot(hx, hz);
    if (hl > 0.001) {
        const int fx = static_cast<int>(std::floor(feet.x + hx / hl * 0.8));
        const int fz = static_cast<int>(std::floor(feet.z + hz / hl * 0.8));
        const int fy = static_cast<int>(std::floor(feet.y));
        const auto below1 = blockAt({fx, fy - 1, fz});
        const auto below2 = blockAt({fx, fy - 2, fz});
        if (!below1.empty() && isAir(below1) && !below2.empty() && isAir(below2)) {
            bool waterBelow = false;
            for (int y = fy - 1; y >= fy - 24; --y) {
                const auto id = blockAt({fx, y, fz});
                if (id.empty())
                    break;
                if (contains(localPart(id), "water")) {
                    waterBelow = true;
                    break;
                }
                if (!isAir(id))
                    break;
            }
            if (!waterBelow) {
                pressKey(kKeyW, false);
                note("cliff guard: target unreachable on foot");
                blacklistCluster(target_, targetId_);
                hasTarget_ = false;
                phase_ = Phase::Scan;
                scanCursor_ = 0;
                bestDist2_ = 1e18;
                return;
            }
        }
    }
    if (independent_) {
        for (auto key : {kKeyW, kKeyA, kKeyS, kKeyD, kKeySpace, kKeySprint})
            pressKey(key, false);
        if (++navTicks_ % 40 == 0) {
            planFailTick_ = tickCount_ - 200;
            pathFor_ = {INT_MIN, 0, 0};
            if (++replans_ >= 3) {
                note("navigate: no safe terrain route; choosing another target");
                blacklistCluster(target_, targetId_);
                hasTarget_ = false;
                phase_ = Phase::Scan;
                scanCursor_ = 0;
                bestDist2_ = 1e18;
            }
        }
        return;
    }
    pressKey(kKeyW, true);
    pressKey(kKeySprint, dist > 6.0); // legacy straight-line travel sprints too
    // Stuck handling: jump, then mine through the blocker.
    ++navTicks_;
    if (navTicks_ % 10 == 0) {
        const double moved = std::hypot(feet.x - lastX_, feet.z - lastZ_);
        if (moved < 0.12) {
            if (++stuckTicks_ == 1) {
                pressKey(kKeySpace, true);
            } else if (stuckTicks_ >= 3) {
                pressKey(kKeySpace, false);
                if (!tryMineThrough(feet)) {
                    if (chainWaitingCobble_)
                        ++cobbleNavFails_;
                    blacklist_.insert(target_);
                    hasTarget_ = false;
                    pressKey(kKeyW, false);
                    phase_ = Phase::Scan;
                    scanCursor_ = 0;
                    bestDist2_ = 1e18;
                    return;
                }
            }
        } else {
            stuckTicks_ = 0;
            pressKey(kKeySpace, false);
        }
        lastX_ = feet.x;
        lastZ_ = feet.z;
    }
    if (navTicks_ > 20 * 45) { // ~45s per target is plenty
        note("navigate timeout; blacklist target");
        if (chainWaitingCobble_)
            ++cobbleNavFails_;
        blacklistCluster(target_, targetId_);
        hasTarget_ = false;
        pressKey(kKeyW, false);
        pressKey(kKeySpace, false);
        phase_ = Phase::Scan;
        scanCursor_ = 0;
        bestDist2_ = 1e18;
    }
}

} // namespace automine::client
