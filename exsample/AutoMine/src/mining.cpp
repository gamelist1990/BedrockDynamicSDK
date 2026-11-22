#include "auto_mine.hpp"

namespace automine::client {

bool AutoMineMod::tryMineThrough(bedrock::sdk::Vec3Value feet) {
    float yaw{}, pitch{};
    if (!rotation(yaw, pitch))
        return false;
    const double rad = yaw * 3.141592653589793 / 180.0;
    const int fx = static_cast<int>(std::floor(feet.x - std::sin(rad) * 1.0));
    const int fz = static_cast<int>(std::floor(feet.z + std::cos(rad) * 1.0));
    const int fy = static_cast<int>(std::floor(feet.y));
    for (int y : {fy, fy + 1}) {
        const auto id = blockAt({fx, y, fz});
        if (!id.empty() && isDiggableCell(id)) {
            subTarget_ = {fx, y, fz};
            subId_ = localPart(id);
            savedTarget_ = target_;
            savedId_ = targetId_;
            target_ = subTarget_;
            targetId_ = subId_;
            hasTarget_ = true;
            phase_ = Phase::Mine;
            mineTicks_ = 0;
            note("mine-through: " + subId_);
            return true;
        }
    }
    return false;
}

void AutoMineMod::tickMine(bedrock::sdk::Vec3Value feet) {
    for (auto key : {kKeyW, kKeyA, kKeyS, kKeyD, kKeySneak, kKeySprint})
        pressKey(key, false);
    setUse(false);
    // Mining from the water is fine, but stay at the surface:
    // without jump held the player sinks mid-mine and drowns.
    pressKey(kKeySpace, inWaterNow(feet));
    if (!(pickupTarget_ == target_)) {
        pickupTarget_ = target_;
        pickupItem_ = dropOf(targetId_);
        pickupBase_ = pickupItem_.empty() ? 0 : invCountOf(pickupItem_);
    }
    const auto current = blockAt(target_);
    if (!current.empty() && (isAir(current) || localPart(current) != targetId_)) {
        setAttack(false);
        miningProgress_.clear();
        ++mined_;
        escapeStreak_ = 0; // real progress: the water patch is workable
        chargeToolUse();
        note("mined " + targetId_ + " total=" + std::to_string(mined_));
        if (ascendDig_) {
            ascendDig_ = false;
            hasTarget_ = false;
            mineTicks_ = 0;
            ascendStuck_ = 0;
            phase_ = Phase::Ascend;
            return;
        }
        if (scaffoldDig_) {
            // The scaffold's ceiling block is gone: restore the
            // real target and hand control back to the climb.
            scaffoldDig_ = false;
            target_ = scaffoldFor_;
            targetId_ = scaffoldId_;
            hasTarget_ = true;
            mineTicks_ = 0;
            scaffoldRiseTick_ = tickCount_;
            phase_ = Phase::Navigate;
            return;
        }
        if (!savedId_.empty() && pathDigResume_ && !contains(targetId_, "_ore")) {
            pathDigResume_ = false;
            target_ = savedTarget_;
            targetId_ = savedId_;
            savedId_.clear();
            mineTicks_ = 0;
            navTicks_ = 0;
            phase_ = Phase::Navigate;
            return;
        }
        if (!savedId_.empty()) {
            if (!contains(targetId_, "_ore") && targetId_ != "leaves") {
                target_ = savedTarget_;
                targetId_ = savedId_;
                savedId_.clear();
                mineTicks_ = 0;
                navTicks_ = 0;
                stuckTicks_ = 0;
                mineOccluder_ = {INT_MIN, 0, 0};
                mineOccluderStreak_ = 0;
                path_.clear();
                pathDig_.clear();
                pathBridge_.clear();
                pathParkour_.clear();
                pathPillar_.clear();
                pathIdx_ = 0;
                pathFor_ = {INT_MIN, 0, 0};
                phase_ = Phase::Navigate;
                note("mine-through: detour cleared, restoring " + targetId_);
                return;
            }
            if (contains(targetId_, "leaves")) {
                target_ = savedTarget_;
                targetId_ = savedId_;
                savedId_.clear();
                mineTicks_ = 0;
                return;
            }
        }
        startCollection();
        return;
    }
    if (!current.empty() && !ensureTool(current)) {
        setAttack(false);
        miningProgress_.pause(tickCount_);
        enterNeedTool();
        return;
    }
    if (independent_) {
        if (miningProgress_.track(target_, tickCount_))
            mineAttemptStance_ = {static_cast<int>(std::floor(feet.x)),
                                  static_cast<int>(std::floor(feet.y)),
                                  static_cast<int>(std::floor(feet.z))};
        if (miningProgress_.stalled(tickCount_)) {
            note("mine: no net destroy progress for 60 ticks; peak=" +
                 std::to_string(miningProgress_.peak()) + " rejecting stance " +
                 std::to_string(mineAttemptStance_.x) + "," +
                 std::to_string(mineAttemptStance_.y) + "," +
                 std::to_string(mineAttemptStance_.z));
            setAttack(false);
            miningProgress_.clear(); // the next stance gets a fresh trial
            mineTicks_ = navTicks_ = 0;
            if (ascendDig_) {
                ascendDig_ = false;
                hasTarget_ = false;
                ascendStuck_ += 20 * 12;
                phase_ = Phase::Ascend;
            } else if (scaffoldDig_) {
                scaffoldDig_ = false;
                target_ = scaffoldFor_;
                targetId_ = scaffoldId_;
                scaffoldRiseTick_ = tickCount_ - 200;
                phase_ = Phase::Navigate;
            } else {
                restoreMineThroughGoal();
                if (!(mineRejectedFor_ == target_))
                    mineRejectedStances_.clear();
                mineRejectedFor_ = target_;
                mineRejectedStances_.insert(mineAttemptStance_);
                endScaffold();
                phase_ = Phase::Navigate;
            }
            invalidateRoute();
            return;
        }
    }
    if (current.empty()) {
        if (!independent_)
            setAttack(false);
        return; // no native action until the block is readable again
    }
    bedrock::sdk::Vec3Value eye;
    if (!eyePos(eye)) {
        if (!independent_)
            setAttack(false);
        return;
    }
    const auto miningHit =
        automine::miningPoint({eye.x, eye.y, eye.z}, {target_.x, target_.y, target_.z});
    const bedrock::sdk::Vec3Value center{static_cast<float>(miningHit.x), static_cast<float>(miningHit.y),
                               static_cast<float>(miningHit.z)};
    if (!automine::inMiningReach({eye.x, eye.y, eye.z}, {target_.x, target_.y, target_.z})) {
        // A momentary loss of reach does not start another build route.
        // No target hit is sent this tick; the stance watchdog owns recovery.
        if (independent_)
            return;
        setAttack(false);
        if (ascendDig_) {
            ascendDig_ = false;
            hasTarget_ = false;
            mineTicks_ = 0;
            phase_ = Phase::Ascend;
            return;
        }
        if (scaffoldDig_) {
            scaffoldDig_ = false;
            target_ = scaffoldFor_;
            targetId_ = scaffoldId_;
            hasTarget_ = true;
            mineTicks_ = 0;
            phase_ = Phase::Navigate;
            return;
        }
        if (pathDigResume_ && !savedId_.empty()) {
            target_ = savedTarget_;
            targetId_ = savedId_;
            savedId_.clear();
            pathDigResume_ = false;
        }
        invalidateRoute();
        phase_ = Phase::Navigate;
        return;
    }
    // Occlusion: if another solid block blocks the ray to the target, mine
    // that block first (dig-through), then resume the original target.
    {
        BlockPos3 occluder;
        if (!lineOfSight(eye, target_, occluder)) {
            // Don't turn a transient aim/position change into a new dig
            // target while an accepted destroy session owns this block.
            if (independent_ && nativeMining_ && nativeMiningCell_ == target_)
                return;
            const auto occId = blockAt(occluder);
            if (chainTablePlaced_ && occluder.x == chainTablePos_.x &&
                occluder.y == chainTablePos_.y && occluder.z == chainTablePos_.z) {
                note("target occluded by the crafting table; rescan");
                setAttack(false);
                pathDigResume_ = false;
                blacklist_.insert(savedId_.empty() ? target_ : savedTarget_);
                if (!savedId_.empty()) {
                    targetId_ = savedId_;
                    savedId_.clear();
                }
                hasTarget_ = false;
                phase_ = Phase::Scan;
                scanCursor_ = 0;
                bestDist2_ = 1e18;
                return;
            }
            if (!occId.empty() && localPart(occId) != "bedrock") {
                setAttack(false);
                if (savedId_.empty()) {
                    savedTarget_ = target_;
                    savedId_ = targetId_;
                }
                if (mineOccluder_ == occluder) {
                    ++mineOccluderStreak_;
                } else {
                    mineOccluder_ = occluder;
                    mineOccluderStreak_ = 1;
                }
                if (mineOccluderStreak_ > 12) {
                    mineOccluderStreak_ = 0;
                    target_ = savedTarget_;
                    targetId_ = savedId_;
                    savedId_.clear();
                    hasTarget_ = true;
                    mineTicks_ = 0;
                    navTicks_ = 0;
                    path_.clear();
                    pathDig_.clear();
                    pathBridge_.clear();
                    pathParkour_.clear();
                    pathPillar_.clear();
                    pathIdx_ = 0;
                    pathFor_ = {INT_MIN, 0, 0};
                    blacklist_.insert(occluder);
                    note("mine-through: repeated occluder abandoned; replanning");
                    phase_ = Phase::Navigate;
                    return;
                }
                target_ = occluder;
                targetId_ = localPart(occId);
                mineTicks_ = 0;
                note("occluded; mining through " + targetId_);
                return;
            }
        }
    }
    const bool facing = faceToward(eye, center, attackHeld_ ? 10.f : 5.f);
    if (facing)
        mineAimTick_ = tickCount_;
    if (facing && !attackHeld_) {
        setAttack(true);
        if (attackHeld_)
            note("attack start");
        else if (mineTicks_ % 40 == 0)
            note("attack send failed");
    }
    if (!independent_ && ++mineTicks_ > 20 * 45) {
        if (ascendDig_) {
            note("ascend dig: mine timeout, back to ascend");
            ascendDig_ = false;
            setAttack(false);
            hasTarget_ = false;
            mineTicks_ = 0;
            ascendStuck_ += 20 * 12;
            phase_ = Phase::Ascend;
            return;
        }
        if (scaffoldDig_) {
            // Charge the failed cap dig to the climb's rise
            // budget so it gives up instead of looping forever.
            note("scaffold dig: mine timeout, back to scaffold");
            scaffoldDig_ = false;
            setAttack(false);
            target_ = scaffoldFor_;
            targetId_ = scaffoldId_;
            hasTarget_ = true;
            mineTicks_ = 0;
            scaffoldRiseTick_ = tickCount_ - 200;
            phase_ = Phase::Navigate;
            return;
        }
        note("mine timeout; blacklist");
        pathDigResume_ = false;
        setAttack(false);
        blacklistCluster(savedId_.empty() ? target_ : savedTarget_,
                         savedId_.empty() ? targetId_ : savedId_);
        hasTarget_ = false;
        if (!savedId_.empty()) {
            targetId_ = savedId_;
            savedId_.clear();
        }
        phase_ = Phase::Scan;
        scanCursor_ = 0;
        bestDist2_ = 1e18;
    }
}

void AutoMineMod::blacklistCluster(const BlockPos3 &center, const std::string &id) {
    blacklist_.insert(center);
    for (int dx = -2; dx <= 2; ++dx)
        for (int dy = -2; dy <= 2; ++dy)
            for (int dz = -2; dz <= 2; ++dz) {
                if (!dx && !dy && !dz)
                    continue;
                const BlockPos3 c{center.x + dx, center.y + dy, center.z + dz};
                if (localPart(blockAt(c)) == id)
                    blacklist_.insert(c);
            }
}

void AutoMineMod::restoreMineThroughGoal() {
    pathDigResume_ = false; // any Collect/Escape exit ends the route resume
    if (!savedId_.empty()) {
        target_ = savedTarget_;
        targetId_ = savedId_;
        savedId_.clear();
    }
}

void AutoMineMod::endScanOverride() {
    if (scanOverrideHeld_) {
        targetId_ = scanSavedId_;
        scanOverrideHeld_ = false;
    }
    scanIds_.clear();
}

void AutoMineMod::startGather(std::vector<std::string> blockIds, std::vector<std::string> countIds,
                              int want, bool hunt) {
    gatherActive_ = true;
    gatherDone_ = false;
    gatherFailed_ = false;
    gatherCountIds_ = std::move(countIds);
    gatherWant_ = want;
    gatherHunt_ = hunt;
    gatherDeadline_ = tickCount_ + 20 * 60 * 25;
    scanIds_ = std::move(blockIds);
    phase_ = Phase::Scan;
    scanCursor_ = 0;
    bestDist2_ = 1e18;
    hasTarget_ = false;
    emptyScans_ = 0;
    note("gather: start, want " + std::to_string(want));
}

bool AutoMineMod::lavaAdjacent(const BlockPos3 &cell) {
    static constexpr BlockPos3 kDirs[6] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                           {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    for (const auto &d : kDirs)
        if (contains(localPart(blockAt({cell.x + d.x, cell.y + d.y, cell.z + d.z})), "lava"))
            return true;
    return false;
}

int AutoMineMod::digCell(const BlockPos3 &cell, bedrock::sdk::Vec3Value feet, bool toolOptional) {
    const auto id = blockAt(cell);
    if (!(digPos_ == cell) || digBlockId_ != id) {
        digPos_ = cell;
        digBlockId_ = id;
        digTicks_ = 0;
        digLavaChecked_ = false;
    }
    if (id.empty() || isAir(id)) {
        setAttack(false);
        digTicks_ = 0;
        return 1;
    }
    if (isPassableCell(id)) {
        setAttack(false);
        digTicks_ = 0;
        return 1;
    }
    if (!digLavaChecked_) {
        digLavaChecked_ = true;
        if (lavaAdjacent(cell)) {
            setAttack(false);
            note("dig: lava-adjacent cell refused");
            return 2;
        }
    }
    const bool toolReady = ensureTool(localPart(id));
    digHandAllowed_ =
        toolOptional && !contains(localPart(id), "_ore") && localPart(id) != "ancient_debris";
    if (toolSyncSlot_ >= 0) {
        setAttack(false);
        return 0;
    }
    if (!toolReady && !digHandAllowed_) {
        setAttack(false);
        return 2;
    }
    if (!toolReady && digTicks_ == 0)
        note("dig: no usable tool; clearing placement pocket by hand (no drop required)");
    bedrock::sdk::Vec3Value eye;
    if (!eyePos(eye))
        return 0;
    const auto miningHit = automine::miningPoint({eye.x, eye.y, eye.z}, {cell.x, cell.y, cell.z});
    const bedrock::sdk::Vec3Value center{static_cast<float>(miningHit.x), static_cast<float>(miningHit.y),
                               static_cast<float>(miningHit.z)};
    if (!automine::inMiningReach({eye.x, eye.y, eye.z}, {cell.x, cell.y, cell.z})) {
        setAttack(false);
        return 2;
    }
    if (!attackHeld_) {
        const bool facing = faceToward(eye, center, 5.f);
        if (facing)
            setAttack(true);
    }
    if (++digTicks_ > 20 * 30) {
        setAttack(false);
        digTicks_ = 0;
        return 2;
    }
    return 0;
}

} // namespace automine::client
