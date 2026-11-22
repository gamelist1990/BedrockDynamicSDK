#include "auto_mine.hpp"

namespace automine::client {

void AutoMineMod::tickDeathWatch() {
    const std::string screen = topScreenName();
    if (contains(screen, "world_loading"))
        joinArmPending_ = true;
    const bool aliveSig = screen == "hud_screen" && game_.gameplayActive();
    if ((screen == "hud_screen" || screen == "hud_crosshair_screen") && joinArmPending_) {
        joinArmPending_ = false;
        joinGuardTicks_ = 20 * 60;
        joinGuardNoted_ = false;
        joinDryTicks_ = 0;
        joinSurfaced_ = false;
    }
    if (aliveSig) {
        deathSigTicks_ = 0;
        bedrock::sdk::Vec3Value f;
        if (feetPos(f)) {
            lastAlivePos_ = f;
            hasLastAlivePos_ = true;
        }
        if (deathPending_) {
            deathPending_ = false;
            if (hasDeathPos_)
                startRecovery();
        }
        return;
    }
    const bool deathSig = screen == "ui://./hbui/gameplay.html" && !game_.gameplayActive();
    if (!deathSig || !hasLastAlivePos_) {
        deathSigTicks_ = 0;
        return;
    }
    if (++deathSigTicks_ < 20)
        return; // 1 s sustained: not a transient
    if (!deathPending_) {
        deathPending_ = true;
        hasDeathPos_ = true;
        deathPos_ = lastAlivePos_;
        note("death: detected at " + std::to_string(static_cast<int>(deathPos_.x)) + "," +
             std::to_string(static_cast<int>(deathPos_.y)) + "," +
             std::to_string(static_cast<int>(deathPos_.z)) + "; respawning");
        reply("死んじゃった…リスポーンして荷物を取りに戻るね");
        resetRun();
    }
    if ((tickCount_ % 30) == 1)
        pressKey(0x0D, true);
    if ((tickCount_ % 30) == 3)
        pressKey(0x0D, false);
}

void AutoMineMod::startRecovery() {
    recoverActive_ = true;
    recoverTicks_ = 0;
    recoverSweepTicks_ = 0;
    recoverStuckTicks_ = 0;
    recoverLastPos_ = deathPos_;
    phase_ = Phase::Recover;
    savedId_.clear();
    target_ = {static_cast<int>(std::floor(deathPos_.x)),
               static_cast<int>(std::floor(deathPos_.y)),
               static_cast<int>(std::floor(deathPos_.z))};
    hasTarget_ = true;
    targetId_ = "corpse_run";
    pathFor_ = {INT_MIN, 0, 0};
    path_.clear();
    note("recover: heading to the death point");
    reply("リスポーンしたよ。荷物を取りに戻るね");
}

void AutoMineMod::recoverDone(bool arrived) {
    releaseInputs();
    recoverActive_ = false;
    hasDeathPos_ = false;
    hasTarget_ = false;
    phase_ = Phase::Idle;
    note(arrived ? "recover: back at the death point" : "recover: gave up");
    reply(arrived ? "荷物の場所に戻ったよ" : "荷物の回収はできなかった…");
}

void AutoMineMod::tickRecover(const bedrock::sdk::Vec3Value &feet) {
    ++recoverTicks_;
    targetId_ = "corpse_run"; // dig detours may have overwritten it
    if (recoverTicks_ > 20 * 240) {
        recoverDone(false);
        return;
    } // 4 min cap
    const double dx = deathPos_.x - feet.x;
    const double dy = deathPos_.y - feet.y;
    const double dz = deathPos_.z - feet.z;
    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const double moved = std::hypot(feet.x - recoverLastPos_.x, feet.z - recoverLastPos_.z);
    if (moved > 1.0) {
        recoverLastPos_ = feet;
        recoverStuckTicks_ = 0;
    } else if (++recoverStuckTicks_ > 20 * 45 && dist > 6.0) {
        recoverDone(false);
        return;
    }
    if (dist <= 4.5) {
        // Final sweep: step onto the spot and linger so the drops
        // are picked up.
        if (++recoverSweepTicks_ > 100) {
            recoverDone(true);
            return;
        }
        bedrock::sdk::Vec3Value eye;
        if (eyePos(eye))
            faceAim(eye, deathPos_, 12.f, 0.3f);
        pressKey(kKeyW, dist > 0.8);
        pressKey(kKeySpace, inWaterNow(feet));
        return;
    }
    target_ = BlockPos3{static_cast<int>(std::floor(deathPos_.x)),
                        static_cast<int>(std::floor(deathPos_.y)),
                        static_cast<int>(std::floor(deathPos_.z))};
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
        const bedrock::sdk::Vec3Value eye{feet.x, feet.y + 1.62f, feet.z};
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
    bedrock::sdk::Vec3Value eye;
    if (!eyePos(eye))
        return;
    faceAim(eye, deathPos_, 12.f, 0.3f);
    pressKey(kKeyW, true);
    pressKey(kKeySpace, inWaterNow(feet) || headInWater(feet));
}

bool AutoMineMod::tickJoinGuard(const bedrock::sdk::Vec3Value &feet) {
    if (joinGuardTicks_ <= 0)
        return false;
    --joinGuardTicks_;
    if (headInWater(feet)) {
        joinDryTicks_ = 0;
        if (!joinGuardNoted_) {
            joinGuardNoted_ = true;
            note("join guard: head underwater after join, surfacing");
            reply("水の中にいたから浮上するね");
        }
        pressKey(kKeySpace, true);
        return true;
    }
    if (joinGuardNoted_) {
        pressKey(kKeySpace, false);
        if (++joinDryTicks_ >= 10 && !joinSurfaced_) {
            joinSurfaced_ = true;
            note("join guard: surfaced, breathing (watching until the window ends)");
        }
        return joinDryTicks_ < 10; // keep control until breathing is confirmed
    }
    if (++joinDryTicks_ >= 100)
        joinGuardTicks_ = 0;
    return false;
}

bool AutoMineMod::tickIdleGuards(const bedrock::sdk::Vec3Value &feet) {
    if (deathPending_)
        return false; // the death watch owns the client now
    const std::string screen = topScreenName();
    if (screen != lastIdleScreen_) {
        lastIdleScreen_ = screen;
        if (!screen.empty() && screen != "hud_screen")
            note("idle screen: '" + screen + "'");
    }
    if (screen.empty() || screen == "hud_screen" || screen == "hud_crosshair_screen") {
        lastAlivePos_ = feet;
        hasLastAlivePos_ = true;
    }
    if (tickJoinGuard(feet))
        return true;
    if (game_.gameplayActive() && (tickCount_ % 4) == 0) {
        if (auto mob = scanHostile(feet, 12.0)) {
            const double dy = std::abs(mob->center.y - feet.y);
            if (dy <= 4.0 || mob->dist <= 6.0) {
                enterCombat(*mob);
                tickCombat(feet);
                return true;
            }
        }
    }
    return false;
}

bool AutoMineMod::headInWater(const bedrock::sdk::Vec3Value &feet) {
    void *src = region();
    if (!src)
        return false;
    bedrock::sdk::BlockPos bp;
    bp.x = static_cast<int>(std::floor(feet.x));
    bp.y = static_cast<int>(std::floor(feet.y + 1.62f));
    bp.z = static_cast<int>(std::floor(feet.z));
    return contains(world_.getBlock(src, bp).identifier, "water");
}

bool AutoMineMod::inWaterNow(const bedrock::sdk::Vec3Value &feet) {
    const auto feetId =
        blockAt({static_cast<int>(std::floor(feet.x)), static_cast<int>(std::floor(feet.y)),
                 static_cast<int>(std::floor(feet.z))});
    return contains(localPart(feetId), "water") || headInWater(feet);
}

void AutoMineMod::startEscape() {
    ++escapeStreak_;
    if (combatEid_) {
        combatIgnore_[combatEid_] = tickCount_ + 20 * 60;
        combatEid_ = 0;
        note("combat: abandoned by surfacing");
    }
    const bool collecting = !collectDropId_.empty();
    if (!collecting && !savedId_.empty()) {
        target_ = savedTarget_;
        targetId_ = savedId_;
        savedId_.clear();
        hasTarget_ = true;
    }
    if (!collecting && hasTarget_) {
        blacklist_.insert(target_);
        hazardZones_.push_back({target_.x, target_.y, target_.z, 6});
        hasTarget_ = false;
    }
    setAttack(false);
    pressKey(kKeyW, false);
    pressKey(kKeyA, false);
    pressKey(kKeyS, false);
    pressKey(kKeyD, false);
    phase_ = Phase::Escape;
    escapeTicks_ = 0;
    dryTicks_ = 0;
    submergedTicks_ = 0;
    if (collecting) {
        resetCollectionSweep();
        note("escape: head underwater; surfacing before resuming pickup");
        reply("水に潜っちゃったから浮上して回収を続けるね");
    } else {
        note("escape: head underwater; abandoning target and surfacing");
        reply("水に潜っちゃったから浮上するね。そのブロックは諦めるよ");
    }
}

void AutoMineMod::tickEscape(const bedrock::sdk::Vec3Value &feet) {
    ++escapeTicks_;
    pressKey(kKeySpace, true); // hold jump: swim up / bob at the surface
    if (!headInWater(feet)) {
        pressKey(kKeyW, false);
        if (++dryTicks_ >= 10 || escapeTicks_ > 240) {
            pressKey(kKeySpace, false);
            phase_ = collectDropId_.empty() ? Phase::Scan : Phase::Collect;
            scanCursor_ = 0;
            bestDist2_ = 1e18;
        }
    } else {
        dryTicks_ = 0;
        if (escapeTicks_ > 100 && lastDryValid_) {
            bedrock::sdk::Vec3Value eye;
            if (eyePos(eye))
                faceToward(eye, {lastDryPos_.x, eye.y, lastDryPos_.z}, 5.f);
            pressKey(kKeyW, true);
        }
    }
    if (escapeTicks_ > 400 && phase_ == Phase::Escape) {
        pressKey(kKeySpace, false);
        pressKey(kKeyW, false);
        phase_ = collectDropId_.empty() ? Phase::Scan : Phase::Collect;
        scanCursor_ = 0;
        bestDist2_ = 1e18;
    }
}

} // namespace automine::client
