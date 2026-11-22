#include "auto_mine.hpp"

namespace automine::client {

void AutoMineMod::enterAscend(int surfY, const std::string &replyText,
                              const std::string &noteText) {
    releaseInputs();
    gatherActive_ = false;
    ascendActive_ = true;
    phase_ = Phase::Ascend;
    ascendTicks_ = 0;
    ascendLastY_ = INT_MAX;
    ascendStuck_ = 0;
    ascendPullTicks_ = 0;
    ascendTargetY_ = surfY;
    ascendOpenSky_ = lastSurfaceY_ == INT_MAX;
    ascendNoteY_ = INT_MIN;
    ascendDig_ = false;
    ascendCenterActive_ = false;
    ascendCenterGiveUp_ = false;
    ascendStepTicks_ = 0;
    ascendStepY_ = INT_MAX;
    ascendShiftActive_ = false;
    ascendStairLatch_ = false;
    ascendOpenTicks_ = 0;
    ascendDirFails_ = 0;
    ascendDirX_ = 1;
    ascendDirZ_ = 0;
    ascendShaftTicks_ = 0;
    ascendShaftBestDist_ = 1e18;
    reply(replyText);
    note(noteText);
}

void AutoMineMod::rotateAscendDir() {
    const int ndx = ascendDirZ_, ndz = -ascendDirX_;
    ascendDirX_ = ndx;
    ascendDirZ_ = ndz;
}

void AutoMineMod::tickAscend(bedrock::sdk::Vec3Value feet) {
    ++ascendTicks_;
    const int fy = static_cast<int>(std::floor(feet.y));
    bool arrived = fy >= ascendTargetY_;
    if (arrived && ascendOpenSky_) {
        const int fx = static_cast<int>(std::floor(feet.x));
        const int fz = static_cast<int>(std::floor(feet.z));
        arrived = isAir(blockAt({fx, fy + 3, fz})) && isAir(blockAt({fx, fy + 8, fz}));
    }
    if (arrived) {
        releaseInputs();
        ascendActive_ = false;
        note("ascend: back at the surface (y=" + std::to_string(fy) + ")");
        reply("地上に戻ったよ。道具を作り直すね");
        hasTarget_ = false;
        if (expeditionActive()) {
            if (expNeedTier_ >= 4 && !hasPickaxeTier(4) && countAllItems("iron_ingot") < 3 &&
                countAllItems("raw_iron") < 3)
                enterExpStage(ExpStage::ToIronLevel);
            else
                enterExpStage(expNeedTier_ >= 4 ? ExpStage::IronPick : ExpStage::StonePick);
            if (phase_ == Phase::Ascend) {
                phase_ = Phase::Scan;
                scanCursor_ = 0;
                bestDist2_ = 1e18;
            }
        } else {
            phase_ = Phase::Scan;
            scanCursor_ = 0;
            bestDist2_ = 1e18;
        }
        return;
    }
    if (ascendLastY_ == INT_MAX) {
        ascendLastRiseTick_ = tickCount_;
        ascendShifts_ = 0;
    }
    if (ascendLastY_ == INT_MAX || fy > ascendLastY_) {
        ascendLastY_ = fy;
        ascendStuck_ = 0;
        ascendLastRiseTick_ = tickCount_;
    }
    if (fy >= ascendNoteY_ + 10) {
        ascendNoteY_ = fy;
        note("ascend: y=" + std::to_string(fy));
    }
    const bool shaftWalk = descendShaftCell_.x != INT_MIN && fy <= descendShaftCell_.y + 2;
    if (shaftWalk)
        ascendLastRiseTick_ = tickCount_;
    if ((!shaftWalk && ++ascendStuck_ > 20 * 50) || ascendTicks_ > 20 * 1800) {
        note("ascend fail: stuck=" + std::to_string(ascendStuck_) +
             " ticks=" + std::to_string(ascendTicks_) + " fy=" + std::to_string(fy) +
             " lastY=" + std::to_string(ascendLastY_));
        releaseInputs();
        expFail("地上へ戻れなかった");
        phase_ = Phase::Idle;
        return;
    }
    if (tickCount_ - ascendLastRiseTick_ > 20 * 40) {
        BlockPos3 shiftCell{INT_MIN, 0, 0};
        if (ascendShifts_ < 4) {
            const int fx = static_cast<int>(std::floor(feet.x));
            const int fz = static_cast<int>(std::floor(feet.z));
            static const int kDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto &d : kDirs) {
                const int nx = fx + d[0], nz = fz + d[1];
                if (!isPassableCell(blockAt({nx, fy, nz})) ||
                    !isPassableCell(blockAt({nx, fy + 1, nz})))
                    continue;
                const auto floorId = blockAt({nx, fy - 1, nz});
                if (isPassableCell(floorId) || contains(localPart(floorId), "water") ||
                    contains(localPart(floorId), "lava"))
                    continue;
                const auto cap = blockAt({nx, fy + 2, nz});
                if (!isPassableCell(cap) && !isDiggableCell(cap))
                    continue;
                shiftCell = {nx, fy, nz};
                break;
            }
        }
        if (shiftCell.x != INT_MIN) {
            ++ascendShifts_;
            ascendLastRiseTick_ = tickCount_;
            ascendShiftCell_ = shiftCell;
            ascendShiftActive_ = true;
            ascendStepTicks_ = 0;
            note("ascend: no rise for 40s; shifting the climb column to " +
                 std::to_string(shiftCell.x) + "," + std::to_string(shiftCell.z));
        } else {
            note("ascend fail: no rise for 40s and no column to shift to"
                 " fy=" +
                 std::to_string(fy));
            releaseInputs();
            expFail("地上へ戻れなかった");
            phase_ = Phase::Idle;
            return;
        }
    }
    const int fx = static_cast<int>(std::floor(feet.x));
    const int fz = static_cast<int>(std::floor(feet.z));
    void *p = player();
    bedrock::sdk::Vec3Value eye;
    if (!eyePos(eye))
        return;
    // Shift walk (from the no-rise budget): step sideways to
    // the chosen neighbouring column, then keep climbing.
    if (ascendShiftActive_) {
        const double tx = ascendShiftCell_.x + .5, tz = ascendShiftCell_.z + .5;
        const double sdx = tx - feet.x, sdz = tz - feet.z;
        if (sdx * sdx + sdz * sdz < 0.15 * 0.15 || ++ascendStepTicks_ > 60) {
            ascendShiftActive_ = false;
            ascendStepTicks_ = 0;
            pressKey(kKeyW, false);
        } else {
            setUse(false);
            pressKey(kKeySpace, false);
            pressKey(kKeySneak, false);
            faceToward(eye, {static_cast<float>(tx), eye.y, static_cast<float>(tz)}, 5.f);
            pressKey(kKeyW, true);
            return;
        }
    }
    if (descendShaftCell_.x != INT_MIN && fy <= descendShaftCell_.y + 2) {
        const double sdx = descendShaftCell_.x + .5 - feet.x;
        const double sdz = descendShaftCell_.z + .5 - feet.z;
        const double sdist = std::hypot(sdx, sdz);
        if (sdist < ascendShaftBestDist_ - 0.5) {
            ascendShaftBestDist_ = sdist;
            ascendShaftTicks_ = 0;
        } else if (++ascendShaftTicks_ > 20 * 60) {
            note("ascend: shaft walk-back made no progress; digging up from here");
            descendShaftCell_.x = INT_MIN;
            ascendShaftTicks_ = 0;
            return;
        }
        if (sdx * sdx + sdz * sdz > 1.2 * 1.2) {
            target_ = descendShaftCell_;
            targetId_ = "ascend_shaft";
            hasTarget_ = true;
            if (!(pathFor_ == target_)) {
                if (!(planFailFor_ == target_) || tickCount_ - planFailTick_ >= 200) {
                    if (!planPath(feet)) {
                        planFailFor_ = target_;
                        planFailTick_ = tickCount_;
                    }
                }
            }
            if (!path_.empty() && pathIdx_ < path_.size() && pathDig_[pathIdx_]) {
                const BlockPos3 &wp = path_[pathIdx_];
                const bedrock::sdk::Vec3Value c{wp.x + .5f, wp.y + .5f, wp.z + .5f};
                const double ddx = c.x - eye.x, ddy = c.y - eye.y, ddz = c.z - eye.z;
                if (ddx * ddx + ddy * ddy + ddz * ddz > 3.2 * 3.2) {
                    faceAim(eye, c, 12.f, 0.3f);
                    pressKey(kKeyW, true);
                    pressKey(kKeySpace, inWaterNow(feet));
                    return;
                }
            }
            if (!path_.empty()) {
                tickFollowPath(feet);
                return;
            }
            faceAim(eye, {descendShaftCell_.x + .5f, feet.y, descendShaftCell_.z + .5f}, 12.f,
                    0.3f);
            pressKey(kKeyW, true);
            pressKey(kKeySpace, inWaterNow(feet));
            return;
        }
    }
    const bool openAbove = isAir(blockAt({fx, fy + 2, fz})) && isAir(blockAt({fx, fy + 3, fz}));
    if (openAbove) {
        if (++ascendOpenTicks_ > 40)
            ascendStairLatch_ = false;
    } else {
        ascendOpenTicks_ = 0;
        ascendStairLatch_ = true;
    }
    if (!openAbove || ascendStairLatch_) {
        if (fy != ascendStepY_) {
            ascendStepY_ = fy;
            ascendStepTicks_ = 0;
        }
        bool havePick = false;
        for (const auto &s : hotbarItems())
            if (s.count > 0 && contains(s.id, "pickaxe")) {
                havePick = true;
                break;
            }
        auto solidDiggable = [this](const std::string &id) {
            return !id.empty() && isDiggableCell(id) && !isPassableCell(id);
        };
        const int nx = fx + ascendDirX_, nz = fz + ascendDirZ_;
        const auto stepId = blockAt({nx, fy, nz});
        const auto bodyId = blockAt({nx, fy + 1, nz});
        const auto headId = blockAt({nx, fy + 2, nz});
        const auto ownCap = blockAt({fx, fy + 2, fz});
        const bool stepSolid = !stepId.empty() && !isPassableCell(stepId) &&
                               !contains(localPart(stepId), "water") &&
                               !contains(localPart(stepId), "lava");
        BlockPos3 digCellPos{INT_MIN, 0, 0};
        std::string digId;
        if (solidDiggable(bodyId)) {
            digCellPos = {nx, fy + 1, nz};
            digId = bodyId;
        } else if (solidDiggable(headId)) {
            digCellPos = {nx, fy + 2, nz};
            digId = headId;
        }
        if (digCellPos.x != INT_MIN && havePick) {
            ascendDirFails_ = 0; // this heading has a step: progress
            note("ascend stair: dig " + localPart(digId) + " at " + std::to_string(digCellPos.x) +
                 "," + std::to_string(digCellPos.y) + "," + std::to_string(digCellPos.z));
            ascendDig_ = true;
            target_ = digCellPos;
            targetId_ = localPart(digId);
            savedId_.clear();
            hasTarget_ = true;
            mineTicks_ = 0;
            setUse(false);
            pressKey(kKeySpace, false);
            phase_ = Phase::Mine;
            return;
        }
        const bool stepClear = stepSolid && isPassableCell(bodyId) && isPassableCell(headId) &&
                               digCellPos.x == INT_MIN;
        if (stepClear) {
            ascendDirFails_ = 0;
            if (ascendStepTicks_ >= 30 && solidDiggable(ownCap) && havePick) {
                note("ascend stair: dig own cap " + localPart(ownCap));
                ascendDig_ = true;
                target_ = {fx, fy + 2, fz};
                targetId_ = localPart(ownCap);
                savedId_.clear();
                hasTarget_ = true;
                mineTicks_ = 0;
                setUse(false);
                pressKey(kKeySpace, false);
                phase_ = Phase::Mine;
                return;
            }
            pressKey(kKeySneak, false);
            faceToward(eye, {nx + .5f, eye.y, nz + .5f}, 5.f);
            pressKey(kKeyW, true);
            pressKey(kKeySpace, true);
            if (++ascendStepTicks_ > 50) {
                ascendStepTicks_ = 0;
                rotateAscendDir();
                note("ascend stair: step failed, rotating");
            }
            if ((ascendTicks_ % 20) == 0)
                note("ascend stair: y=" + std::to_string(feet.y) +
                     " dir=" + std::to_string(ascendDirX_) + "," + std::to_string(ascendDirZ_));
            return;
        }
        if (!stepSolid || digCellPos.x == INT_MIN) {
            bool fallThrough = false;
            if (++ascendStepTicks_ > 10) {
                ascendStepTicks_ = 0;
                rotateAscendDir();
                note("ascend stair: no step this way, rotating");
                if (++ascendDirFails_ >= 4) {
                    ascendDirFails_ = 0;
                    fallThrough = true;
                }
            }
            if (!fallThrough)
                return;
        }
        // A dig is needed but no pickaxe is in the hotbar:
        // fall through to the pillar branch.
    }
    int slot = spareSlot();
    if (slot >= 0)
        ascendPullTicks_ = 0; // the pull budget is per shortage, not per climb
    if (slot < 0) {
        // Pull a spare stack up from the main inventory first.
        if (craftReady_ && ascendPullTicks_ < 600) {
            ++ascendPullTicks_;
            setUse(false);
            pressKey(kKeySpace, false);
            if (prepareBuildingSupply())
                return;
        }
        releaseInputs();
        note("ascend: out of spare blocks");
        expFail("足場ブロックが尽きた");
        phase_ = Phase::Idle;
        return;
    }
    if (!ascendCenterGiveUp_) {
        if (!ascendCenterActive_) {
            ascendCenterActive_ = true;
            ascendCenterCell_ = {static_cast<int>(std::floor(feet.x)), fy,
                                 static_cast<int>(std::floor(feet.z))};
            ascendCenterTicks_ = 0;
        }
        const double ctx = ascendCenterCell_.x + 0.5;
        const double ctz = ascendCenterCell_.z + 0.5;
        const double cdx = ctx - feet.x, cdz = ctz - feet.z;
        if (feet.y - fy < 0.05 && cdx * cdx + cdz * cdz > 0.12 * 0.12) {
            if (++ascendCenterTicks_ > 40) {
                ascendCenterGiveUp_ = true;
                ascendCenterActive_ = false;
                pressKey(kKeyW, false);
                note("ascend: centering gave up, climbing as-is");
            } else {
                setUse(false);
                pressKey(kKeySpace, false);
                pressKey(kKeySneak, false);
                faceToward(eye, {static_cast<float>(ctx), eye.y, static_cast<float>(ctz)}, 5.f);
                pressKey(kKeyW, true);
                return;
            }
        } else {
            ascendCenterActive_ = false;
            pressKey(kKeyW, false);
        }
    }
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
            if (havePick) {
                note("ascend dig: via mine, head=" + capId + " fy=" + std::to_string(fy));
                ascendDig_ = true;
                target_ = capCell;
                targetId_ = localPart(capId);
                savedId_.clear();
                hasTarget_ = true;
                mineTicks_ = 0;
                setUse(false);
                pressKey(kKeySpace, false);
                phase_ = Phase::Mine;
                return;
            }
            // No tool in the hotbar: the jump-place below
            // makes no progress and the stuck timer gives up.
        }
    }
    if (p && world_.selected(p) != slot)
        world_.select(p, static_cast<unsigned>(slot));
    const int pfx = static_cast<int>(std::floor(feet.x));
    const int pfz = static_cast<int>(std::floor(feet.z));
    const bedrock::sdk::Vec3Value aim{pfx + .5f, static_cast<float>(fy), pfz + .5f};
    faceToward(eye, aim, 3.f);
    pressKey(kKeySneak, true);
    pressKey(kKeySpace, true);
    setUse(true);
    if ((ascendTicks_ % 20) == 0)
        note("ascend tick: y=" + std::to_string(feet.y) + " below=" + blockAt({pfx, fy - 1, pfz}) +
             " head=" + blockAt({pfx, fy + 2, pfz}) +
             " slot=" + std::to_string(p ? world_.selected(p) : -1));
}

void AutoMineMod::tickDescend(bedrock::sdk::Vec3Value feet) {
    if (feet.y <= descendTargetY_) {
        setAttack(false);
        descendDone_ = true;
        phase_ = Phase::Idle;
        // Remember the shaft bottom: the surface return walks
        // back to this column and pillars up the open shaft.
        descendShaftCell_ = {static_cast<int>(std::floor(feet.x)),
                             static_cast<int>(std::floor(feet.y)),
                             static_cast<int>(std::floor(feet.z))};
        note("descend: reached y=" + std::to_string(static_cast<int>(feet.y)));
        return;
    }
    {
        const BlockPos3 headCell{static_cast<int>(std::floor(feet.x)),
                                 static_cast<int>(std::floor(feet.y + 1.62)),
                                 static_cast<int>(std::floor(feet.z))};
        const bool headWet = contains(localPart(blockAt(headCell)), "water");
        if (descendBreath_) {
            setAttack(false);
            pressKey(kKeySpace, true);
            if (!headWet) {
                pressKey(kKeySpace, false);
                descendBreath_ = false;
                descendBreathTicks_ = 0;
                descendWetTicks_ = 0;
                if (descendBreathBreaks_ >= 3) {
                    descendDone_ = true;
                    descendCave_ = true;
                    phase_ = Phase::Idle;
                    note("descend: flooded cavern swallows this shaft (repeated breath breaks), "
                         "relocating");
                    return;
                }
                note("descend: breath break over");
            } else if (++descendBreathTicks_ > 20 * 20) {
                pressKey(kKeySpace, false);
                descendBreath_ = false;
                descendBreathTicks_ = 0;
                descendWetTicks_ = 0;
                descendDone_ = true;
                descendCave_ = true;
                phase_ = Phase::Idle;
                floodHold_ = true;
                floodWetTicks_ = 0;
                floodDryTicks_ = 0;
                note("descend: no air above; shaft is flooded, relocating");
                return;
            }
            return;
        }
        if (headWet) {
            if (++descendWetTicks_ > 20 * 3) {
                descendBreath_ = true;
                descendBreathTicks_ = 0;
                ++descendBreathBreaks_;
                digPos_ = {INT_MIN, 0, 0};
                digBlockId_.clear();
                digTicks_ = 0;
                note("descend: surfacing for air");
                return;
            }
        } else
            descendWetTicks_ = 0;
    }
    const int fy = static_cast<int>(std::floor(feet.y));
    if (fy < descendLastY_) {
        descendLastY_ = fy;
        descendStuck_ = 0;
    }
    if (descendEntryY_ == INT_MAX) {
        descendEntryY_ = fy;
        if (lastSurfaceY_ == INT_MAX)
            lastSurfaceY_ = fy;
    }
    const bool inShaft = fy <= descendEntryY_ - 2;
    const int cx = static_cast<int>(std::floor(feet.x + 0.3));
    const int cz = static_cast<int>(std::floor(feet.z + 0.3));
    const bool singleColumn =
        static_cast<int>(std::floor(feet.x - .299)) ==
            static_cast<int>(std::floor(feet.x + .299)) &&
        static_cast<int>(std::floor(feet.z - .299)) == static_cast<int>(std::floor(feet.z + .299));
    if (!descendWide_ && !singleColumn && !inShaft) {
        setAttack(false);
        const double tx = cx + 0.5, tz = cz + 0.5;
        const double dx = tx - feet.x, dz = tz - feet.z;
        if (dx * dx + dz * dz > 0.15 * 0.15) {
            bedrock::sdk::Vec3Value eye;
            if (eyePos(eye))
                faceToward(eye, {static_cast<float>(tx), feet.y, static_cast<float>(tz)}, 0.05f);
            pressKey(kKeyW, true);
        } else {
            pressKey(kKeyW, false);
        }
        if (++descendCenterTicks_ > 20 * 3) {
            pressKey(kKeyW, false);
            descendWide_ = true;
            note("descend: centering failed, digging wide");
        }
        return;
    }
    pressKey(kKeyW, false);
    descendCenterTicks_ = 0;
    std::vector<BlockPos3> cols;
    cols.push_back({cx, fy - 1, cz});
    if ((descendWide_ || !singleColumn) && !inShaft) {
        const int x0 = static_cast<int>(std::floor(feet.x - .299));
        const int x1 = static_cast<int>(std::floor(feet.x + .299));
        const int z0 = static_cast<int>(std::floor(feet.z - .299));
        const int z1 = static_cast<int>(std::floor(feet.z + .299));
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z)
                if (x != cx || z != cz)
                    cols.push_back({x, fy - 1, z});
    }
    for (const auto &c : cols) {
        if (contains(localPart(blockAt(c)), "lava")) {
            descendDone_ = true;
            descendFailed_ = true;
            phase_ = Phase::Idle;
            note("descend: lava directly below");
            return;
        }
    }
    int voidDepth = 0;
    std::string voidFloor;
    bool voidWater = false;
    for (int y = fy - 2; y > fy - 9; --y) {
        const auto id = blockAt({cx, y, cz});
        if (id.empty() || isAir(id))
            ++voidDepth;
        else if (contains(localPart(id), "water")) {
            ++voidDepth;
            voidWater = true;
        } else {
            voidFloor = localPart(id);
            break;
        }
    }
    bool deepWater = false;
    if (voidWater) {
        int waterCells = 0;
        bool floorFound = false;
        for (int y = fy - 2; y > fy - 18; --y) {
            const auto wid = blockAt({cx, y, cz});
            if (contains(localPart(wid), "water"))
                ++waterCells;
            else if (!wid.empty() && !isAir(wid)) {
                floorFound = true;
                break;
            }
        }
        deepWater = !floorFound || waterCells > 5;
    }
    if (voidDepth >= 3 && voidFloor != "lava" && (!voidFloor.empty() || voidWater) &&
        !(deepWater && fy > descendTargetY_ + 10)) {
        if (descendDropNoteY_ != fy) {
            descendDropNoteY_ = fy;
            note("descend: opening below (" +
                 (voidWater && voidFloor.empty()
                      ? std::string("water")
                      : "floor " + std::to_string(voidDepth + 1) + " down") +
                 "), dropping in");
        }
        // Fall through to the dig loop: breaking the centre
        // column drops the player onto the floor.
    } else if (voidDepth >= 3) {
        setAttack(false);
        descendDone_ = true;
        descendCave_ = true;
        phase_ = Phase::Idle;
        note(deepWater ? "descend: flooded cavern below, stopping the shaft"
                       : "descend: cave opening below, stopping the shaft");
        return;
    }
    bool refused = false;
    for (const auto &c : cols) {
        const auto id = blockAt(c);
        if (id.empty() || isAir(id) || contains(localPart(id), "water"))
            continue;
        if (digCell(c, feet) == 2) {
            refused = true;
            continue;
        }
        descendStuck_ = 0;
        return; // working on this column
    }
    if (refused) {
        descendDone_ = true;
        descendFailed_ = true;
        phase_ = Phase::Idle;
        note("descend: dig refused below");
        return;
    }
    // Nothing solid under the footprint: falling (or swimming).
    setAttack(false);
    if (++descendStuck_ > 20 * 20) {
        descendDone_ = true;
        descendFailed_ = true;
        phase_ = Phase::Idle;
        note("descend: no progress, giving up");
    }
}

void AutoMineMod::tickHunt(bedrock::sdk::Vec3Value feet) {
    const int fx = static_cast<int>(std::floor(feet.x));
    const int fy = static_cast<int>(std::floor(feet.y));
    const int fz = static_cast<int>(std::floor(feet.z));
    if (!huntStepActive_) {
        huntStep_ = {fx + huntDirX_, fy, fz + huntDirZ_};
        huntStepActive_ = true;
    }
    const BlockPos3 ahead = huntStep_;
    if (huntStage_ == 0) {
        // No floor ahead means a cave edge or water: end the leg and
        // let the supervisor pick a new direction.
        const auto floorId = blockAt({ahead.x, ahead.y - 1, ahead.z});
        if (floorId.empty() || isAir(floorId) || contains(localPart(floorId), "water") ||
            contains(localPart(floorId), "lava")) {
            releaseInputs();
            huntLegDone_ = true;
            huntBlocked_ = true;
            phase_ = Phase::Idle;
            note("hunt: no floor ahead, leg over");
            return;
        }
        const BlockPos3 head{ahead.x, ahead.y + 1, ahead.z};
        const auto headId = blockAt(head);
        if (headId.empty() || isAir(headId)) {
            huntStage_ = 1;
            return;
        }
        if (digCell(head, feet) != 0)
            huntStage_ = 1;
        return;
    }
    if (huntStage_ == 1) {
        const auto footId = blockAt(ahead);
        if (footId.empty() || isAir(footId)) {
            huntStage_ = 2;
            huntTicks_ = 0;
            return;
        }
        if (digCell(ahead, feet) != 0) {
            huntStage_ = 2;
            huntTicks_ = 0;
        }
        return;
    }
    // Advance one cell into the cleared space.
    bedrock::sdk::Vec3Value eye;
    if (!eyePos(eye))
        return;
    const bedrock::sdk::Vec3Value want{ahead.x + .5f, feet.y + 1.0f, ahead.z + .5f};
    faceToward(eye, want, 0.05f);
    pressKey(kKeyW, true);
    const int nx = static_cast<int>(std::floor(feet.x));
    const int nz = static_cast<int>(std::floor(feet.z));
    if ((nx == ahead.x && nz == ahead.z &&
         std::hypot(ahead.x + .5 - feet.x, ahead.z + .5 - feet.z) < .45) ||
        ++huntTicks_ > 70) {
        pressKey(kKeyW, false);
        huntStage_ = 0;
        huntStepActive_ = false;
        if (huntTicks_ > 70) { // could not advance: treat as blocked
            huntLegDone_ = true;
            huntBlocked_ = true;
            phase_ = Phase::Idle;
            note("hunt: advance blocked, leg over");
            return;
        }
        if (++huntCells_ >= 6) {
            huntLegDone_ = true;
            phase_ = Phase::Idle;
            note("hunt: leg complete");
        }
    }
}

} // namespace automine::client
