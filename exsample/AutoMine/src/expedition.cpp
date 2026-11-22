#include "auto_mine.hpp"

namespace automine::client {

bool AutoMineMod::expeditionActive() const {
    return expStage_ != ExpStage::None;
}

bool AutoMineMod::maybeStartExpedition() {
    if (expeditionActive() || chainActive_)
        return false;
    if (classForBlock(targetId_) != ToolClass::Pickaxe)
        return false;
    needTier_ = requiredTier(targetId_);
    const auto have = chainCounts();
    if (needTier_ >= 4 && countIn(have, "iron_ingot") < 3 && !hasPickaxeTier(4)) {
        startExpedition(hasPickaxeTier(2) ? ExpStage::ToIronLevel : ExpStage::Wood);
        return true;
    }
    if (needTier_ >= 4 && !hasPickaxeTier(4) && countIn(have, "iron_ingot") >= 3) {
        startExpedition(ExpStage::IronPick);
        return true;
    }
    if (!hasPickaxeTier(1)) {
        const int wood =
            countIn(have, "_log") + countIn(have, "_wood") + countIn(have, "planks") / 4;
        if (wood < 3) {
            startExpedition(ExpStage::Wood);
            return true;
        }
    }
    return false;
}

int AutoMineMod::heldPickaxeTier() { // tier of the pickaxe in the selected slot, else 0
    void *p = player();
    if (!p)
        return 0;
    const int slot = world_.selected(p);
    if (slot < 0 || slot > 8)
        return 0;
    const auto slots = hotbarMemoryItems();
    int tier = 0;
    return classForItem(slots[static_cast<std::size_t>(slot)].id, tier) == ToolClass::Pickaxe ? tier
                                                                                              : 0;
}

bool AutoMineMod::hasPickaxeTier(int minTier) {
    for (const auto &s : craft_.snapshot()) {
        if (!s.count)
            continue;
        int t{};
        if (classForItem(localPart(s.item), t) == ToolClass::Pickaxe && t >= minTier)
            return true;
    }
    return false;
}

void AutoMineMod::startExpedition(ExpStage stage) {
    expStage_ = stage;
    expNeedTier_ = needTier_;
    expGoalItem_ = goalItem_;
    expGoalCount_ = goalCount_;
    huntDirX_ = 1;
    huntDirZ_ = 0;
    huntBlocked_ = false;
    smeltRetries_ = 0;
    reply("材料を自分で集めるところからやるね");
    note("expedition: start (needTier " + std::to_string(expNeedTier_) + ")");
    enterExpStage(stage);
}

void AutoMineMod::expFail(const std::string &why) {
    note("expedition failed: " + why);
    reply("自律調達はここまで（" + why + "）");
    ascendActive_ = false;
    expStage_ = ExpStage::None;
    gatherActive_ = false;
    endScanOverride();
    releaseInputs();
    if (phase_ != Phase::Craft)
        phase_ = Phase::Idle;
}

void AutoMineMod::enterExpStage(ExpStage stage) {
    expStage_ = stage;
    note("expedition: stage " + std::to_string(static_cast<int>(stage)));
    switch (stage) {
    case ExpStage::Wood: {
        descendDivertUsed_ = false;
        descendPending_ = false;
        static const std::vector<std::string> kLogs = {
            "oak_log",      "birch_log",    "spruce_log", "jungle_log",  "acacia_log",
            "dark_oak_log", "mangrove_log", "cherry_log", "pale_oak_log"};
        reply("まず木を集めるね");
        startGather(kLogs, kLogs, 20, true);
        return;
    }
    case ExpStage::StonePick:
        needTier_ = 2;
        wantPickaxeChain_ = true;
        chainRetries_ = 0;
        chainNextRetryTick_ = 0;
        (void)tryStartPickaxeChain();
        return;
    case ExpStage::ToIronLevel:
    case ExpStage::ToDiamondLevel: {
        descendCaveRetries_ = 0;
        descendResumePending_ = false;
        const int needTier = stage == ExpStage::ToIronLevel ? 2 : 4;
        const int threshold = stage == ExpStage::ToIronLevel ? 60 : 120;
        if (!descendDivertUsed_) {
            const int best = bestPickaxeRemaining(needTier);
            if (best >= 0 && best < threshold && !hasSparePickaxe(needTier)) {
                descendDivertUsed_ = true;
                needTier_ = needTier;
                wantPickaxeChain_ = true;
                if (tryStartPickaxeChain(true)) {
                    descendPending_ = true;
                    note("expedition: pickaxe durability low (~" + std::to_string(best) +
                         " left); crafting a fresh one first");
                    reply("ツルハシがボロボロだから、先に新しいのを作るね");
                    return;
                }
            }
        }
        ensureSwordPrep();
        if (chainActive_) {
            descendPending_ = true;
            return;
        } // sword chain first
        beginDescend(stage);
        return;
    }
    case ExpStage::HuntIron:
        reply("鉄鉱石を探すね");
        startGather({"iron_ore", "deepslate_iron_ore"}, {"raw_iron"}, 3, true);
        return;
    case ExpStage::Cobble:
        startGather({"stone"}, {"cobblestone"}, 8, true);
        return;
    case ExpStage::Smelt:
        reply("鉄を精錬するね");
        startSmelt("raw_iron", 3);
        return;
    case ExpStage::IronPick:
        needTier_ = 4;
        wantPickaxeChain_ = true;
        chainRetries_ = 0;
        chainNextRetryTick_ = 0;
        (void)tryStartPickaxeChain();
        return;
    case ExpStage::HuntOre: {
        reply("ダイヤを探すね");
        const int want = expGoalCount_ > 0 ? expGoalCount_ : 10;
        startGather({"diamond_ore", "deepslate_diamond_ore"}, {"diamond"}, want, true);
        return;
    }
    case ExpStage::None:
        return;
    }
}

void AutoMineMod::beginDescend(ExpStage stage) {
    const int target = stage == ExpStage::ToIronLevel ? 16 : -53;
    descendTargetY_ = target;
    descendDone_ = false;
    descendFailed_ = false;
    descendCave_ = false;
    descendLastY_ = INT_MAX;
    descendStuck_ = 0;
    digPos_ = {INT_MIN, 0, 0};
    descendWide_ = false;
    descendCenterTicks_ = 0;
    descendWetTicks_ = 0;
    descendBreathTicks_ = 0;
    descendBreath_ = false;
    descendBreathBreaks_ = 0;
    pressKey(kKeySpace, false);
    descendEntryY_ = INT_MAX; // captured on the first descend tick
    phase_ = Phase::Descend;
    reply(stage == ExpStage::ToIronLevel ? "鉄を探しに降りるね" : "ダイヤ層まで降りるね");
}

bool AutoMineMod::descendCaveResume(ExpStage stage, bedrock::sdk::Vec3Value feet) {
    if (!descendCave_)
        return false;
    const int fy = static_cast<int>(std::floor(feet.y));
    if (fy <= descendTargetY_ + 16)
        return false;
    if (descendCaveRetries_ >= 6)
        return false;
    ++descendCaveRetries_;
    descendDone_ = false;
    descendCave_ = false;
    descendResumePending_ = true;
    huntStepActive_ = false;
    huntStage_ = 0;
    huntCells_ = 0;
    huntLegDone_ = false;
    huntBlocked_ = false;
    digPos_ = {INT_MIN, 0, 0};
    phase_ = Phase::Hunt;
    note("expedition: cavern stop at y=" + std::to_string(fy) + ", still above the y=" +
         std::to_string(descendTargetY_) + " band; relocating and descending again");
    return true;
}

bool AutoMineMod::descendResumeTick(ExpStage stage) {
    if (!descendResumePending_)
        return false;
    if (huntLegDone_ && phase_ == Phase::Idle) {
        huntLegDone_ = false;
        if (huntBlocked_) {
            huntBlocked_ = false;
            const int ndx = huntDirZ_, ndz = -huntDirX_;
            huntDirX_ = ndx;
            huntDirZ_ = ndz;
        }
        descendResumePending_ = false;
        beginDescend(stage);
    }
    return true;
}

void AutoMineMod::tickExpedition(bedrock::sdk::Vec3Value feet) {
    if (!expeditionActive())
        return;
    // Gather outcomes are stage transitions wherever they land.
    if (gatherDone_ || gatherFailed_) {
        const bool ok = gatherDone_;
        gatherDone_ = gatherFailed_ = false;
        if (!ok) {
            expFail("素材が集まらなかった");
            return;
        }
        switch (expStage_) {
        case ExpStage::Wood:
            enterExpStage(ExpStage::StonePick);
            return;
        case ExpStage::HuntIron:
            if (countAllItems("cobblestone") >= 8)
                enterExpStage(ExpStage::Smelt);
            else
                enterExpStage(ExpStage::Cobble);
            return;
        case ExpStage::Cobble:
            enterExpStage(ExpStage::Smelt);
            return;
        case ExpStage::HuntOre: {
            if (expGoalCount_ > 0) {
                reply("目標達成！ " + expGoalItem_ + " を " + std::to_string(expGoalCount_) +
                      " 個集めたよ");
                note("expedition: goal reached");
                goalCount_ = 0;
            }
            expStage_ = ExpStage::None;
            phase_ = Phase::Idle;
            hasTarget_ = false;
            return;
        }
        default:
            return;
        }
    }
    if (tickCount_ % 20 == 0 && !chainActive_ && !ascendDig_ && !ascendActive_ &&
        (expStage_ == ExpStage::HuntIron || expStage_ == ExpStage::HuntOre) &&
        phase_ != Phase::Ascend && phase_ != Phase::Craft && phase_ != Phase::Smelt &&
        phase_ != Phase::Descend) {
        const int need = expStage_ == ExpStage::HuntIron ? 2 : 4;
        const int fy = static_cast<int>(std::floor(feet.y));
        const int surfY = lastSurfaceY_ == INT_MAX ? 70 : lastSurfaceY_;
        if (fy < surfY - 8 && !hasPickaxeTier(need) && !hasSparePickaxe(need)) {
            needTier_ = need;
            wantPickaxeChain_ = true;
            if (!tryStartPickaxeChain() && totalSpareCount() >= surfY - fy) {
                enterAscend(surfY, "道具が作れないから、地上に戻って仕切り直すね",
                            "ascend: returning to surface y=" + std::to_string(surfY));
                return;
            }
        } else if (fy < surfY - 8 && !hasSparePickaxe(need)) {
            const int best = bestPickaxeRemaining(need);
            if (best >= 0 && best < 50) {
                needTier_ = need;
                wantPickaxeChain_ = true;
                if (!tryStartPickaxeChain()) {
                    enterAscend(surfY, "ツルハシが壊れそうだから、壊れる前に地上に戻るね",
                                "ascend: pickaxe nearly worn out (~" + std::to_string(best) +
                                    " left); returning before it breaks, surface y=" +
                                    std::to_string(surfY));
                    return;
                }
            }
        }
    }
    if (gatherActive_ || chainActive_)
        return;
    switch (expStage_) {
    case ExpStage::StonePick:
        if (hasPickaxeTier(2)) {
            enterExpStage(expNeedTier_ >= 4 ? ExpStage::ToIronLevel : ExpStage::None);
            if (expStage_ == ExpStage::None) {
                phase_ = Phase::Scan;
            }
        } else if (tickCount_ % 20 == 0) {
            needTier_ = 2;
            wantPickaxeChain_ = true;
            (void)tryStartPickaxeChain();
        }
        return;
    case ExpStage::ToIronLevel:
        if (descendResumeTick(ExpStage::ToIronLevel))
            return;
        if (descendPending_) {
            if (!chainActive_) {
                descendPending_ = false;
                beginDescend(ExpStage::ToIronLevel);
            }
            return;
        }
        if (descendDone_) {
            if (descendCaveResume(ExpStage::ToIronLevel, feet))
                return;
            enterExpStage(ExpStage::HuntIron);
        }
        return;
    case ExpStage::Smelt:
        if (smeltDone_) {
            smeltDone_ = false;
            enterExpStage(ExpStage::IronPick);
        } else if (smeltFailed_) {
            smeltFailed_ = false;
            if (++smeltRetries_ <= 1)
                enterExpStage(ExpStage::Smelt);
            else
                expFail("精錬できなかった");
        }
        return;
    case ExpStage::IronPick:
        if (hasPickaxeTier(4))
            enterExpStage(ExpStage::ToDiamondLevel);
        else if (tickCount_ % 20 == 0) {
            needTier_ = 4;
            wantPickaxeChain_ = true;
            (void)tryStartPickaxeChain();
        }
        return;
    case ExpStage::ToDiamondLevel:
        if (descendResumeTick(ExpStage::ToDiamondLevel))
            return;
        if (descendPending_) {
            if (!chainActive_) {
                descendPending_ = false;
                beginDescend(ExpStage::ToDiamondLevel);
            }
            return;
        }
        if (descendDone_) {
            if (descendCaveResume(ExpStage::ToDiamondLevel, feet))
                return;
            enterExpStage(ExpStage::HuntOre);
        }
        return;
    default:
        return;
    }
}

} // namespace automine::client
