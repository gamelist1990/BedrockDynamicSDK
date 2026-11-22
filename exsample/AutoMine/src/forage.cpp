#include "auto_mine.hpp"

namespace automine::client {

void AutoMineMod::startForage(int want) {
    resetRun();
    forageActive_ = true;
    forageWant_ = std::clamp(want, 1, 16);
    forageStage_ = ForageStage::Hunt;
    forageEid_ = 0;
    forageType_ = 0;
    forageEngaged_ = false;
    forageTicks_ = 0;
    forageKills_ = 0;
    forageCollectTicks_ = 0;
    forageNoAnimalTicks_ = 0;
    forageLastHitTick_ = 0;
    forageIgnore_.clear();
    forageSmeltStarted_ = false;
    forageTriedRaw_.clear();
    forageRawId_.clear();
    forageCookedId_.clear();
    forageEatStep_ = 0;
    forageEatTicks_ = 0;
    forageEatBase_ = 0;
    forageEaten_ = 0;
    phase_ = Phase::Forage;
    reply("食料を調達するね（肉" + std::to_string(forageWant_) + "個が目標）");
    note("forage: start want=" + std::to_string(forageWant_));
}

void AutoMineMod::forageFail(const std::string &why) {
    note("forage failed: " + why);
    reply("食料調達はここまで（" + why + "）");
    forageActive_ = false;
    releaseInputs();
    setUse(false);
    setAttack(false);
    if (phase_ == Phase::Forage)
        phase_ = Phase::Idle;
}

void AutoMineMod::forageDone() {
    note("forage: done kills=" + std::to_string(forageKills_) +
         " eaten=" + std::to_string(forageEaten_));
    reply("狩って焼いて食べたよ（" + std::to_string(forageKills_) + "頭狩って、" +
          std::to_string(forageEaten_) + "個食べた）");
    forageActive_ = false;
    releaseInputs();
    setUse(false);
    setAttack(false);
    if (phase_ == Phase::Forage)
        phase_ = Phase::Idle;
}

int AutoMineMod::forageRawTotal() {
    return countAllItems("beef") + countAllItems("porkchop") + countAllItems("chicken") +
           countAllItems("mutton");
}

void AutoMineMod::tickForage(const bedrock::sdk::Vec3Value &feet) {
    if (!forageActive_)
        return;
    ++forageTicks_;
    if (forageTicks_ > 20 * 60 * 10) {
        forageFail("時間切れ");
        return;
    }
    const bedrock::sdk::Vec3Value eye{feet.x, feet.y + 1.62f, feet.z};
    if (forageStage_ == ForageStage::Hunt) {
        if (attackPulse_ > 0 && --attackPulse_ == 0)
            setAttack(false);
        if (attackCd_ > 0)
            --attackCd_;
        if (forageRawTotal() >= forageWant_) {
            releaseInputs();
            forageStage_ = ForageStage::Cook;
            forageSmeltStarted_ = false;
            note("forage: enough raw meat, cooking");
            return;
        }
        if (forageEid_) {
            if (forageTicks_ - forageLastHitTick_ > 20 * 20) {
                note("forage: animal unreachable, moving on");
                forageIgnore_[forageEid_] = forageTicks_ + 20 * 60;
                forageEid_ = 0;
                forageEngaged_ = false;
                releaseInputs();
                return;
            }
            auto mob = scanPassive(feet, 48.0, forageEid_);
            if (mob) {
                forageCentre_ = mob->center;
                BlockPos3 hitCell{};
                const BlockPos3 mobCell{static_cast<int>(std::floor(mob->center.x)),
                                        static_cast<int>(std::floor(mob->center.y)),
                                        static_cast<int>(std::floor(mob->center.z))};
                const bool los = lineOfSight(eye, mobCell, hitCell);
                const double reach = distToMob(eye, *mob);
                const bool wet = inWaterNow(feet);
                if (reach > 2.9 || !los) {
                    faceAim(eye, mob->center, 5.f, .35f);
                    pressKey(kKeyW, true);
                    pressKey(kKeySprint, mob->dist > 6.0);
                    pressKey(kKeySpace, wet || mob->center.y > feet.y + .6f);
                } else {
                    pressKey(kKeyW, false);
                    pressKey(kKeySprint, false);
                    pressKey(kKeySpace, wet);
                    faceToward(eye, mob->center, 3.f);
                    if (attackCd_ == 0) {
                        const Weapon w = bestMelee();
                        if (w.slot >= 0)
                            selectSlot(w.slot);
                        setAttack(true);
                        attackPulse_ = 2;
                        attackCd_ = 11;
                        forageEngaged_ = true;
                        forageLastHitTick_ = forageTicks_;
                    }
                }
                return;
            }
            // The animal is gone (killed, or fled out of range):
            // walk onto its last spot so the drops get picked up.
            forageEid_ = 0;
            if (forageEngaged_)
                ++forageKills_;
            forageEngaged_ = false;
            forageCollectTicks_ = 50;
            note("forage: animal down, collecting drops");
            return;
        }
        if (forageCollectTicks_ > 0) {
            --forageCollectTicks_;
            const double dx = forageCentre_.x - feet.x;
            const double dz = forageCentre_.z - feet.z;
            if (dx * dx + dz * dz > 1.2) {
                faceAim(eye, forageCentre_, 5.f, .35f);
                pressKey(kKeyW, true);
                pressKey(kKeySpace, inWaterNow(feet));
            } else {
                pressKey(kKeyW, false);
            }
            if (forageCollectTicks_ == 0)
                pressKey(kKeyW, false);
            return;
        }
        for (auto it = forageIgnore_.begin(); it != forageIgnore_.end();) {
            if (it->second <= forageTicks_)
                it = forageIgnore_.erase(it);
            else
                ++it;
        }
        if (auto mob = scanPassive(feet, 48.0)) {
            if (forageIgnore_.count(mob->eid)) {
            } else {
                forageEid_ = mob->eid;
                forageType_ = mob->type;
                forageCentre_ = mob->center;
                forageEngaged_ = false;
                forageLastHitTick_ = forageTicks_;
                forageNoAnimalTicks_ = 0;
                note("forage: hunting type=" + std::to_string(mob->type) + " (" +
                     rawMeatOf(mob->type) + ")");
                return;
            }
        }
        if (++forageNoAnimalTicks_ > 20 * 60 && forageRawTotal() > 0) {
            releaseInputs();
            forageStage_ = ForageStage::Cook;
            forageSmeltStarted_ = false;
            note("forage: no more animals nearby, cooking what we have");
            return;
        }
        // Walkabout: keep moving in the hunt direction (turning
        // every ~8 s), hopping onto single blocks in the way.
        if ((forageTicks_ % 160) == 0) {
            const int ndx = huntDirZ_, ndz = -huntDirX_;
            huntDirX_ = ndx;
            huntDirZ_ = ndz;
        }
        const bedrock::sdk::Vec3Value ahead{feet.x + huntDirX_ * 8.f, feet.y, feet.z + huntDirZ_ * 8.f};
        faceAim(eye, ahead, 5.f, .3f);
        pressKey(kKeyW, true);
        const BlockPos3 fwdCell{static_cast<int>(std::floor(feet.x)) + huntDirX_,
                                static_cast<int>(std::floor(feet.y)),
                                static_cast<int>(std::floor(feet.z)) + huntDirZ_};
        const bool blocked = isSolidCell(localPart(blockAt(fwdCell)));
        const bool headroom =
            !isSolidCell(localPart(blockAt({fwdCell.x, fwdCell.y + 1, fwdCell.z})));
        pressKey(kKeySpace, blocked && headroom);
        return;
    }
    if (forageStage_ == ForageStage::Cook) {
        if (inWaterNow(feet)) {
            if ((forageTicks_ % 100) == 0) {
                const int ndx = huntDirZ_, ndz = -huntDirX_;
                huntDirX_ = ndx;
                huntDirZ_ = ndz;
            }
            const bedrock::sdk::Vec3Value ahead{feet.x + huntDirX_ * 8.f, feet.y + 1.f,
                                      feet.z + huntDirZ_ * 8.f};
            faceAim(eye, ahead, 5.f, .3f);
            pressKey(kKeyW, true);
            pressKey(kKeySpace, true);
            return;
        }
        pressKey(kKeyW, false);
        pressKey(kKeySpace, false);
        if (!forageSmeltStarted_) {
            // Cook the largest raw stack (one kind per smelt run),
            // skipping kinds already exposed as inventory ghosts.
            static const char *kRaws[4] = {"beef", "porkchop", "chicken", "mutton"};
            int bestCount = 0;
            for (const char *r : kRaws) {
                if (forageTriedRaw_.count(r))
                    continue;
                const int c = countAllItems(r);
                if (c > bestCount) {
                    bestCount = c;
                    forageRawId_ = r;
                }
            }
            if (bestCount <= 0) {
                forageFail("肉が無かった");
                return;
            }
            forageCookedId_ = cookedMeatOf(forageRawId_);
            startSmelt(forageRawId_, std::min(bestCount, forageWant_));
            forageSmeltStarted_ = true;
            return;
        }
        if (smeltDone_) {
            smeltDone_ = false;
            forageStage_ = ForageStage::Eat;
            forageEatStep_ = 0;
            forageEatTicks_ = 0;
            phase_ = Phase::Forage;
            note("forage: cooked " + forageCookedId_);
            return;
        }
        if (smeltFailed_) {
            smeltFailed_ = false;
            if (smeltFailReason_.find("材料") != std::string::npos && !forageRawId_.empty()) {
                forageTriedRaw_.insert(forageRawId_);
                forageSmeltStarted_ = false;
                note("forage: " + forageRawId_ + " was a ghost; trying another meat");
                return;
            }
            forageFail("焼けなかった");
            return;
        }
        return; // the smelt machinery owns the phase until it lands in Idle
    }
    // Eat.
    if (moveStage_ != MoveStage::None) {
        (void)ensureHotbarTick(moveItem_);
        return;
    }
    const int cooked = countAllItems(forageCookedId_);
    if (cooked <= 0 || forageEaten_ >= 2) {
        forageDone();
        return;
    }
    int slot = -1;
    const auto bar = hotbarItems();
    for (int i = 0; i < 9; ++i) {
        const auto &it = bar[static_cast<std::size_t>(i)];
        if (it.count && localPart(it.id) == forageCookedId_) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (!moveFailed_)
            (void)ensureHotbarTick(forageCookedId_);
        else
            forageFail("焼けた肉をホットバーに移せなかった");
        return;
    }
    selectSlot(slot);
    if (forageEatStep_ == 0) {
        forageEatBase_ = cooked;
        forageEatStep_ = 1;
        forageEatTicks_ = 0;
        setUse(true);
        return;
    }
    if (++forageEatTicks_ >= 45) {
        setUse(false);
        const int now = countAllItems(forageCookedId_);
        forageEatStep_ = 0;
        if (now < forageEatBase_) {
            ++forageEaten_;
            note("forage: ate " + forageCookedId_ + " (" + std::to_string(forageEaten_) + ")");
        } else {
            // Nothing consumed in 45 ticks of holding use: the
            // hunger bar is full. That is a successful meal state.
            note("forage: not hungry, keeping the rest");
            forageDone();
        }
    }
}

} // namespace automine::client
