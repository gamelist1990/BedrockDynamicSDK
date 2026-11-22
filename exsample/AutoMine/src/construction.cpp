#include "auto_mine.hpp"

namespace automine::client {

void AutoMineMod::enterMineCell(BlockPos3 cell, const std::string &id) {
    savedTarget_ = target_;
    savedId_ = targetId_;
    target_ = cell;
    targetId_ = localPart(id);
    hasTarget_ = true;
    phase_ = Phase::Mine;
    mineTicks_ = 0;
    path_.clear();
    pathDig_.clear();
    pathBridge_.clear();
    pathParkour_.clear();
    pathPillar_.clear();
    pathIdx_ = 0;
    pathFor_ = {INT_MIN, 0, 0};
    pathDigResume_ = false;
    note("mine-through: " + targetId_);
}

void AutoMineMod::enterPathDigCell(BlockPos3 cell, const std::string &id) {
    if (savedId_.empty()) {
        savedTarget_ = target_;
        savedId_ = targetId_;
    }
    target_ = cell;
    targetId_ = localPart(id);
    hasTarget_ = true;
    phase_ = Phase::Mine;
    mineTicks_ = 0;
    pathDigResume_ = true;
    note("path dig: " + targetId_);
}

bool AutoMineMod::tickBridge(bedrock::sdk::Vec3Value feet) {
    std::vector<BlockPos3> run;
    for (std::size_t i = pathIdx_; i < path_.size() && pathBridge_[i] && !pathParkour_[i] &&
                                   !pathPillar_[i] && path_[i].y == path_[pathIdx_].y;
         ++i)
        run.push_back({path_[i].x, path_[i].y - 1, path_[i].z});
    const auto supported = [&](BlockPos3 floor) {
        const auto id = blockAt(floor);
        return isSolidCell(id) && automine::fullCubeSupport(id);
    };
    if (std::all_of(run.begin(), run.end(), supported)) {
        if (bridgeTicks_)
            note("bridge: floor confirmed");
        bridgeTicks_ = 0;
        pressKey(kKeySneak, false);
        setUse(false);
        return false;
    }
    for (auto key : {kKeyW, kKeyA, kKeyS, kKeyD, kKeySpace, kKeySprint})
        pressKey(key, false);
    pressKey(kKeySneak, true);
    setAttack(false);
    setUse(false);
    const auto fail = [&](const char *why) {
        for (const auto &floor : run)
            if (!supported(floor))
                bridgeBlocked_.insert(floor);
        bridgeTicks_ = 0;
        pressKey(kKeySneak, false);
        invalidateRoute();
        note(std::string("bridge: ") + why + ", replanning");
    };
    if (++bridgeTicks_ > 120) {
        fail("no confirmed placement");
        return true;
    }
    if (spareSlot() < 0) {
        if (prepareBuildingSupply())
            return true;
        fail("no usable spare blocks");
        return true;
    }
    bedrock::sdk::Vec3Value eye;
    if (!eyePos(eye))
        return true;
    if (bridgeTicks_ % 3 == 1)
        for (auto it = run.rbegin(); it != run.rend(); ++it)
            if (!supported(*it) && placeLandingPad(*it, eye))
                return true;
    // Approach only across a verified footprint; never walk into an unfinished bridge.
    const auto target = run.front();
    const double dx = target.x + .5 - feet.x, dz = target.z + .5 - feet.z,
                 length = std::hypot(dx, dz);
    if (length > .01) {
        const automine::NavPoint to{feet.x + dx / length * .12, feet.y, feet.z + dz / length * .12};
        automine::TerrainCache readCache([this](automine::PlanCell c) { return planTerrain(c); });
        const auto read = [&](automine::PlanCell p) { return readCache(p); };
        if (automine::walkClear({feet.x, feet.y, feet.z}, to, read)) {
            faceToward(
                eye, {static_cast<float>(target.x + .5), eye.y, static_cast<float>(target.z + .5)},
                5.f);
            pressKey(kKeyW, true);
        }
    }
    return true;
}

void AutoMineMod::tickPillarWaypoint(bedrock::sdk::Vec3Value feet) {
    const auto wp = path_[pathIdx_];
    const auto terrain = [this](automine::PlanCell c) { return planTerrain(c); };
    const BlockPos3 floor{wp.x, wp.y - 1, wp.z};
    const auto support = blockAt(floor);
    setAttack(false);
    setUse(false);
    for (auto key : {kKeyW, kKeyA, kKeyS, kKeyD, kKeySprint})
        pressKey(key, false);
    const bool landed = std::abs(feet.y - wp.y) < .10 &&
                        std::hypot(feet.x - wp.x - .5, feet.z - wp.z - .5) < .24 &&
                        isSolidCell(support) && automine::fullCubeSupport(support) &&
                        automine::supportFraction({feet.x, feet.y, feet.z}, terrain) > .8;
    if (landed) {
        pressKey(kKeySpace, false);
        pressKey(kKeySneak, false);
        if (++pillarLandingTicks_ >= 2) {
            pillarLandingTicks_ = pillarWpTicks_ = 0;
            ++pathIdx_;
            note("pillar: floor and landing confirmed");
        }
        return;
    }
    pillarLandingTicks_ = 0;
    if (pillarWpTicks_ == 0)
        pillarWpBaseY_ = static_cast<int>(std::floor(feet.y));
    const auto fail = [&](const char *why) {
        pillarWpTicks_ = 0;
        pressKey(kKeySpace, false);
        pressKey(kKeySneak, false);
        planBlockedFor_ = target_;
        planBlocked_.insert({wp.x, wp.y, wp.z});
        if (phase_ == Phase::Navigate)
            ++replans_;
        invalidateRoute();
        note(std::string("pillar: ") + why + ", replanning");
    };
    if (++pillarWpTicks_ > 100 || feet.y < pillarWpBaseY_ - 1) {
        fail("failed");
        return;
    }
    for (int y = wp.y; y <= wp.y + 2; ++y) {
        const auto id = blockAt({wp.x, y, wp.z});
        if (!isPassableCell(id) && isDiggableCell(id)) {
            pressKey(kKeySpace, false);
            pressKey(kKeySneak, false);
            enterPathDigCell({wp.x, y, wp.z}, id);
            return;
        }
    }
    if (spareSlot() < 0) {
        if (prepareBuildingSupply())
            return;
        fail("no usable blocks");
        return;
    }
    bedrock::sdk::Vec3Value eye;
    if (!eyePos(eye))
        return;
    if (std::hypot(feet.x - wp.x - .5, feet.z - wp.z - .5) > .15 &&
        automine::supportFraction({feet.x, feet.y, feet.z}, terrain) > .5) {
        faceToward(eye, {wp.x + .5f, eye.y, wp.z + .5f}, 5.f);
        pressKey(kKeyW, true);
        pressKey(kKeySpace, false);
        return;
    }
    pressKey(kKeySneak, true);
    pressKey(kKeySpace, true);
    if (feet.y >= wp.y + .02 && pillarWpTicks_ % 2 == 0) {
        std::string placedId;
        if (placeLandingPad(floor, eye, &placedId) && !placedId.empty())
            placedPillars_[floor] = std::move(placedId);
    }
}

bool AutoMineMod::wallLandingAnchor(BlockPos3 floor) {
    constexpr int dirs[4][2]{{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const auto &d : dirs)
        if (const auto id = blockAt({floor.x + d[0], floor.y, floor.z + d[1]});
            isSolidCell(id) && automine::fullCubeSupport(id))
            return true;
    return false;
}

bool AutoMineMod::placeLandingPad(BlockPos3 floor, bedrock::sdk::Vec3Value eye, std::string *placedId) {
    if (anyScreenUp())
        return false; // placement on an interactive support can push a screen
    const auto existing = blockAt(floor);
    if (isSolidCell(existing) && automine::fullCubeSupport(existing))
        return true;
    const int slot = spareSlot();
    void *p = player();
    if (slot < 0 || !p)
        return false;
    const auto expected = placedId && isAir(existing)
                              ? localPart(buildingHotbar()[static_cast<std::size_t>(slot)].id)
                              : std::string{};
    automine::TerrainCache readCache([this](automine::PlanCell c) { return planTerrain(c); });
    const auto read = [&](automine::PlanCell p) { return readCache(p); };
    const auto anchors = [this](automine::PlanCell c) {
        const auto id = blockAt({c.x, c.y, c.z});
        return isSolidCell(id) && automine::fullCubeSupport(id);
    };
    const auto options =
        automine::placementFaces({floor.x, floor.y, floor.z}, {eye.x, eye.y, eye.z}, read, anchors);
    for (const auto &option : options) {
        if (world_.selected(p) != slot && !world_.select(p, static_cast<unsigned>(slot)))
            return false;
        bool accepted{};
        const auto a = option.anchor;
        const auto h = option.hit;
        (void)world_.build(
            p, world_.gameMode(p), {a.x, a.y, a.z}, option.face, eye,
            {static_cast<float>(h.x), static_cast<float>(h.y), static_cast<float>(h.z)}, &accepted);
        if (accepted) {
            if (placedId)
                *placedId = expected;
            return true;
        }
    }
    return false;
}

bool AutoMineMod::tickParkourWaypoint(bedrock::sdk::Vec3Value feet) {
    const auto wp = path_[pathIdx_];
    setAttack(false);
    setUse(false);
    for (auto key : {kKeyA, kKeyS, kKeyD, kKeySneak, kKeySpace})
        pressKey(key, false);
    if (!(parkourWaypoint_ == wp) || parkourRevision_ != pathRevision_) {
        parkourWaypoint_ = wp;
        parkourRevision_ = pathRevision_;
        parkourStage_ = parkourTicks_ = parkourLandingTicks_ = 0;
        parkourRose_ = false;
        parkourFrom_ = pathIdx_ ? path_[pathIdx_ - 1]
                                : BlockPos3{static_cast<int>(std::floor(feet.x)),
                                            static_cast<int>(std::floor(feet.y)),
                                            static_cast<int>(std::floor(feet.z))};
        parkourLastFeet_ = feet;
        parkourLastTick_ = tickCount_ - 1;
        parkourPadWait_ = false;
        parkourSprintInput_ = false;
    }
    const double dx = wp.x - parkourFrom_.x, dz = wp.z - parkourFrom_.z,
                 distance = std::hypot(dx, dz);
    const auto fail = [&](const char *why) {
        parkourSprintInput_ = false;
        pressKey(kKeyW, false);
        pressKey(kKeySprint, false);
        pressKey(kKeySpace, false);
        planBlockedFor_ = target_;
        planBlocked_.insert({wp.x, wp.y, wp.z});
        if (phase_ == Phase::Collect || (phase_ == Phase::Navigate && navReturn_))
            ++replans_;
        invalidateRoute();
        note(std::string("path: parkour ") + why + ", stage=" + std::to_string(parkourStage_) +
             " sprint=0 jump=0, replanning");
    };
    if (distance < 1 || ++parkourTicks_ > 140 || feet.y < std::min(wp.y, parkourFrom_.y) - 1.2) {
        fail("failed");
        return true;
    }
    const double ux = dx / distance, uz = dz / distance;
    automine::TerrainCache terrainCache([this](automine::PlanCell p) { return planTerrain(p); });
    const auto terrain = [&](automine::PlanCell p) { return terrainCache(p); };
    const double support = automine::supportFraction({feet.x, feet.y, feet.z}, terrain);
    const bool grounded = support > .15;
    const long long elapsed = std::max(1LL, tickCount_ - parkourLastTick_);
    const double forwardSpeed =
        ((feet.x - parkourLastFeet_.x) * ux + (feet.z - parkourLastFeet_.z) * uz) / elapsed;
    parkourLastFeet_ = feet;
    parkourLastTick_ = tickCount_;
    const double along = (feet.x - parkourFrom_.x - .5) * ux + (feet.z - parkourFrom_.z - .5) * uz;
    const double remaining = std::hypot(wp.x + .5 - feet.x, wp.z + .5 - feet.z);
    const BlockPos3 floor{wp.x, wp.y - 1, wp.z};
    const auto supported = [&] {
        const auto id = blockAt(floor);
        return isSolidCell(id) && automine::fullCubeSupport(id);
    };
    if (parkourStage_ >= 2 && parkourRose_ && grounded && support > .8 &&
        std::abs(feet.y - wp.y) < .12 && remaining < .42 && supported()) {
        pressKey(kKeyW, false);
        pressKey(kKeySprint, false);
        parkourSprintInput_ = false;
        if (++parkourLandingTicks_ >= 2) {
            parkourWaypoint_ = {INT_MIN, 0, 0};
            ++pathIdx_;
            note("path: parkour landing confirmed");
            return false;
        }
        return true;
    }
    parkourLandingTicks_ = 0;
    bedrock::sdk::Vec3Value eye;
    if (!eyePos(eye)) {
        pressKey(kKeyW, false);
        pressKey(kKeySprint, false);
        parkourSprintInput_ = false;
        return true;
    }
    const int runup =
        automine::jumpRunup({parkourFrom_.x, parkourFrom_.y, parkourFrom_.z}, {wp.x, wp.y, wp.z});
    if (parkourPadWait_) {
        parkourSprintInput_ = false;
        pressKey(kKeyW, false);
        pressKey(kKeySprint, false);
        if (supported()) {
            parkourPadWait_ = false;
            parkourStage_ = 0;
        } else if (tickCount_ - parkourPadTick_ > 30)
            fail("pad placement unconfirmed");
        return true;
    }
    if (pathBridge_[pathIdx_] && !supported() && parkourStage_ < 2 && parkourTicks_ % 2 == 0 &&
        placeLandingPad(floor, eye)) {
        parkourPadWait_ = true;
        parkourPadTick_ = tickCount_;
        parkourSprintInput_ = false;
        pressKey(kKeyW, false);
        pressKey(kKeySprint, false);
        return true;
    }
    if (parkourStage_ == 0) {
        parkourSprintInput_ = false;
        pressKey(kKeySprint, false);
        const bedrock::sdk::Vec3Value back{static_cast<float>(parkourFrom_.x + .5 - ux * runup), eye.y,
                                 static_cast<float>(parkourFrom_.z + .5 - uz * runup)};
        if (std::hypot(back.x - feet.x, back.z - feet.z) > .23) {
            if (!automine::walkClear({feet.x, feet.y, feet.z},
                                     {back.x, static_cast<double>(parkourFrom_.y), back.z},
                                     terrain)) {
                fail("runway obstructed");
                return true;
            }
            // Legacy key control must finish turning before walking back;
            // independent control resolves the movement heading immediately.
            pressKey(kKeyW, faceToward(eye, back, 5.f));
            pressKey(kKeySprint, false);
            return true;
        }
        if (!grounded) {
            pressKey(kKeyW, false);
            return true;
        }
        if (!automine::jumpClear({parkourFrom_.x, parkourFrom_.y, parkourFrom_.z},
                                 {wp.x, wp.y, wp.z}, terrain)) {
            fail("arc obstructed");
            return true;
        }
        if (!faceToward(eye, {wp.x + .5f, eye.y, wp.z + .5f}, 5.f)) {
            pressKey(kKeyW, false);
            return true;
        }
        parkourStage_ = 1;
        parkourSprintStart_ = controls_.observation().sprintCalls;
        parkourRunTick_ = tickCount_;
        note("path: parkour runup sprint=1 forward=1 jump=0 (hold through flight)");
    }
    parkourSprintInput_ = true;
    faceToward(eye, {wp.x + .5f, eye.y, wp.z + .5f}, 5.f);
    // Do not release sprint near the landing while still airborne. Forward
    // steering is recomputed toward the landing every tick; stop only on support.
    pressKey(kKeyW, true);
    pressKey(kKeySprint, true);
    if (pathBridge_[pathIdx_] && !supported() && parkourStage_ >= 2 && parkourTicks_ % 2 == 0)
        (void)placeLandingPad(floor, eye);
    if (parkourStage_ == 1) {
        const auto sprintCalls = independent_
            ? controls_.observation().sprintCalls - parkourSprintStart_
            : static_cast<unsigned long long>(tickCount_ - parkourRunTick_);
        if (automine::sprintTakeoffReady(grounded, along, forwardSpeed, sprintCalls)) {
            parkourStage_ = 2;
            parkourLaunchTick_ = tickCount_;
            note("path: sprint jump -> " + std::to_string(wp.x) + "," + std::to_string(wp.y) + "," +
                 std::to_string(wp.z) + " speed=" + std::to_string(forwardSpeed) +
                 " along=" + std::to_string(along) + " sprint_calls=" + std::to_string(sprintCalls));
        } else if (along > .38 || !grounded) {
            note("path: parkour takeoff rejected along=" + std::to_string(along) +
                 " speed=" + std::to_string(forwardSpeed) +
                 " grounded=" + std::to_string(grounded) +
                 " sprint_calls=" + std::to_string(sprintCalls));
            fail("missed takeoff window");
            return true;
        }
    }
    const auto drive = automine::parkourDrive(parkourStage_, tickCount_ - parkourLaunchTick_);
    pressKey(kKeyW, drive.forward);
    pressKey(kKeySprint, drive.sprint);
    pressKey(kKeySpace, drive.jump);
    if (parkourStage_ == 2 && feet.y > parkourFrom_.y + .15)
        if (!parkourRose_) {
            parkourRose_ = true;
            note("path: parkour airborne sprint=1 forward=1 speed=" + std::to_string(forwardSpeed));
        }
    if (parkourStage_ == 2 && !parkourRose_ && tickCount_ - parkourLaunchTick_ > 8) {
        fail("did not rise");
        return true;
    }
    return true;
}

bool AutoMineMod::isScaffoldBlock(const std::string &local) {
    return isSpareBlock(local);
}

bool AutoMineMod::scaffoldWanted(bedrock::sdk::Vec3Value feet) {
    // Once committed, keep the climb until it reaches a mining stance or
    // explicitly fails. Don't recommit after a failed mining handoff.
    if (navReturn_ || scaffoldFor_ == target_ || inWaterNow(feet))
        return false;
    const int fy = static_cast<int>(std::floor(feet.y));
    if (target_.y - fy < 4)
        return false; // not out of reach upward
    const double hdx = (target_.x + .5) - feet.x, hdz = (target_.z + .5) - feet.z;
    if (hdx * hdx + hdz * hdz > 6.0 * 6.0)
        return false; // walk closer first
    bool routeUseless = path_.empty() && (planFailFor_ == target_ || replans_ >= 3);
    if (!routeUseless && pathPartial_ && pathIdx_ < path_.size()) {
        // Only pre-empt an incomplete vertical approach at the trunk.
        // A complete walking/pillar route owns its actions.
        bool vertical = true;
        for (std::size_t i = pathIdx_; i < path_.size(); ++i)
            if (std::abs(path_[i].x - target_.x) > 1 || std::abs(path_[i].z - target_.z) > 1) {
                vertical = false;
                break;
            }
        routeUseless = vertical;
    }
    if (!routeUseless)
        return false;
    if (!scaffoldStanceOk(feet))
        return false;
    engageScaffold(feet);
    return true;
}

bool AutoMineMod::scaffoldStanceOk(bedrock::sdk::Vec3Value feet) {
    if (navReturn_ || inWaterNow(feet))
        return false;
    const int fy = static_cast<int>(std::floor(feet.y));
    const double hdx = (target_.x + .5) - feet.x, hdz = (target_.z + .5) - feet.z;
    bool stance = (hdx * hdx + hdz * hdz <= 1.7 * 1.7);
    static constexpr int kDX[4] = {1, -1, 0, 0};
    static constexpr int kDZ[4] = {0, 0, 1, -1};
    for (int i = 0; i < 4 && !stance; ++i) {
        const BlockPos3 cell{target_.x + kDX[i], fy, target_.z + kDZ[i]};
        const auto at = blockAt(cell);
        const auto ground = blockAt({cell.x, fy - 1, cell.z});
        if (!at.empty() && isPassableCell(at) && !ground.empty() && !isAir(ground) &&
            !isPassableCell(ground) && !contains(localPart(ground), "water"))
            stance = true;
    }
    return stance && scaffoldCount() >= 3;
}

void AutoMineMod::engageScaffold(bedrock::sdk::Vec3Value feet) {
    scaffoldActive_ = true;
    scaffoldFor_ = target_;
    scaffoldId_ = targetId_;
    scaffoldTicks_ = 0;
    scaffoldPullTicks_ = 0;
    scaffoldLastY_ = feet.y;
    scaffoldRiseTick_ = tickCount_;
    scaffoldCenterActive_ = false;
    scaffoldCenterGiveUp_ = false;
    path_.clear();
    pathDig_.clear();
    pathBridge_.clear();
    pathParkour_.clear();
    pathPillar_.clear();
    pathIdx_ = 0;
    note("scaffold: climbing to elevated " + targetId_ + " at " + std::to_string(target_.x) + "," +
         std::to_string(target_.y) + "," + std::to_string(target_.z));
}

void AutoMineMod::endScaffold() {
    scaffoldActive_ = false;
    pressKey(kKeySpace, false);
    pressKey(kKeySneak, false);
    pressKey(kKeyW, false);
    setUse(false);
}

void AutoMineMod::scaffoldFail(const std::string &why) {
    note("scaffold: giving up (" + why + ")");
    blacklist_.insert(scaffoldFor_);
    hasTarget_ = false;
    endScaffold();
    releaseInputs();
    phase_ = Phase::Scan;
    scanCursor_ = 0;
    bestDist2_ = 1e18;
}

bool AutoMineMod::tickScaffold(bedrock::sdk::Vec3Value feet) {
    if (!hasTarget_ || !(target_ == scaffoldFor_)) {
        endScaffold();
        return false;
    }
    ++scaffoldTicks_;
    if (scaffoldTicks_ > 20 * 90) {
        scaffoldFail("timeout");
        return true;
    }
    if (feet.y > scaffoldLastY_ + 0.5) {
        scaffoldLastY_ = feet.y;
        scaffoldRiseTick_ = tickCount_;
    }
    if (tickCount_ - scaffoldRiseTick_ > 200) {
        scaffoldFail("no rise");
        return true;
    }
    bedrock::sdk::Vec3Value eye;
    if (!eyePos(eye))
        return true;
    const bedrock::sdk::Vec3Value center{scaffoldFor_.x + .5f, scaffoldFor_.y + .5f, scaffoldFor_.z + .5f};
    const int fy = static_cast<int>(std::floor(feet.y));
    const BlockPos3 stance{static_cast<int>(std::floor(feet.x)), fy,
                           static_cast<int>(std::floor(feet.z))};
    const bool rejectedStance = mineRejectedFor_ == target_ && mineRejectedStances_.count(stance);
    const auto supported = [&] {
        return automine::supportFraction({feet.x, feet.y, feet.z},
                                         [this](automine::PlanCell c) { return planTerrain(c); }) > .5;
    };
    if (automine::miningHandoffReady({eye.x, eye.y, eye.z}, {target_.x, target_.y, target_.z},
                                   false, rejectedStance, supported)) {
        // In reach: hand over to the normal mine phase.
        endScaffold();
        phase_ = Phase::Mine;
        mineTicks_ = 0;
        return true;
    }
    // Walk to the base of the target column first (ground-level
    // approach); the climb itself starts beside the column.
    const double hdx = center.x - feet.x, hdz = center.z - feet.z;
    if (std::hypot(hdx, hdz) > 1.7 && feet.y < scaffoldFor_.y - 3) {
        setUse(false);
        pressKey(kKeySpace, false);
        pressKey(kKeySneak, false);
        faceToward(eye, {center.x, eye.y, center.z}, 5.f);
        pressKey(kKeyW, true);
        scaffoldCenterActive_ = false;
        return true;
    }
    pressKey(kKeyW, false);
    if (feet.y - fy < 0.05) {
        const int fx = static_cast<int>(std::floor(feet.x));
        const int fz = static_cast<int>(std::floor(feet.z));
        const int minCx = static_cast<int>(std::floor(feet.x - 0.3));
        const int maxCx = static_cast<int>(std::floor(feet.x + 0.3));
        const int minCz = static_cast<int>(std::floor(feet.z - 0.3));
        const int maxCz = static_cast<int>(std::floor(feet.z + 0.3));
        BlockPos3 capCell{INT_MIN, 0, 0};
        std::string capId;
        for (int pass = 0; pass < 2 && capId.empty(); ++pass)
            for (int cx = minCx; cx <= maxCx && capId.empty(); ++cx)
                for (int cz = minCz; cz <= maxCz; ++cz) {
                    const bool feetCol = (cx == fx && cz == fz);
                    if ((pass == 0) != feetCol)
                        continue;
                    const auto id = blockAt({cx, fy + 2, cz});
                    if (isDiggableCell(id) && !isPassableCell(id)) {
                        capCell = {cx, fy + 2, cz};
                        capId = id;
                        break;
                    }
                }
        if (!capId.empty()) {
            bool havePick = false;
            for (const auto &s : hotbarItems())
                if (s.count > 0 && contains(s.id, "pickaxe")) {
                    havePick = true;
                    break;
                }
            const bool stoneCap = contains(localPart(capId), "stone") ||
                                  contains(localPart(capId), "deepslate") ||
                                  contains(localPart(capId), "ore");
            if (havePick || !stoneCap) {
                note("scaffold dig: via mine, head=" + capId);
                scaffoldDig_ = true;
                target_ = capCell;
                targetId_ = localPart(capId);
                savedId_.clear();
                hasTarget_ = true;
                mineTicks_ = 0;
                setUse(false);
                pressKey(kKeySpace, false);
                phase_ = Phase::Mine;
                return true;
            }
        }
    }
    // Supply: largest scaffold stack in the hotbar; pull from the
    // main inventory when the hotbar runs dry.
    const int slot = scaffoldSlot();
    if (slot < 0) {
        if (scaffoldCount() > 0 && scaffoldPullTicks_ < 200) {
            ++scaffoldPullTicks_;
            setUse(false);
            pressKey(kKeySpace, false);
            if (prepareBuildingSupply())
                return true;
        }
        scaffoldFail("足場ブロックが尽きた");
        return true;
    }
    void *p = player();
    if (p && world_.selected(p) != slot)
        world_.select(p, static_cast<unsigned>(slot));
    if (!scaffoldCenterGiveUp_) {
        if (!scaffoldCenterActive_) {
            scaffoldCenterActive_ = true;
            scaffoldCenterCell_ = {static_cast<int>(std::floor(feet.x)), fy,
                                   static_cast<int>(std::floor(feet.z))};
            scaffoldCenterTicks_ = 0;
        }
        const double ctx = scaffoldCenterCell_.x + 0.5;
        const double ctz = scaffoldCenterCell_.z + 0.5;
        const double cdx = ctx - feet.x, cdz = ctz - feet.z;
        if (feet.y - fy < 0.05 && cdx * cdx + cdz * cdz > 0.12 * 0.12) {
            if (++scaffoldCenterTicks_ > 40) {
                scaffoldCenterGiveUp_ = true;
                scaffoldCenterActive_ = false;
                pressKey(kKeyW, false);
                note("scaffold: centering gave up, climbing as-is");
            } else {
                setUse(false);
                pressKey(kKeySpace, false);
                pressKey(kKeySneak, false);
                faceToward(eye, {static_cast<float>(ctx), eye.y, static_cast<float>(ctz)}, 5.f);
                pressKey(kKeyW, true);
                return true;
            }
        } else {
            scaffoldCenterActive_ = false;
            pressKey(kKeyW, false);
        }
    }
    const int pfx = static_cast<int>(std::floor(feet.x));
    const int pfz = static_cast<int>(std::floor(feet.z));
    const bedrock::sdk::Vec3Value aim{pfx + .5f, static_cast<float>(fy), pfz + .5f};
    faceToward(eye, aim, 3.f);
    pressKey(kKeySneak, true);
    pressKey(kKeySpace, true);
    setUse(true);
    if ((scaffoldTicks_ % 20) == 0)
        note("scaffold tick: y=" + std::to_string(feet.y) +
             " target y=" + std::to_string(scaffoldFor_.y));
    return true;
}

} // namespace automine::client
