#include "auto_mine.hpp"

namespace automine::client {

int AutoMineMod::maxDurabilityOf(const std::string &local) {
    if (!contains(local, "_pickaxe") && !contains(local, "_axe") && !contains(local, "_sword") &&
        !contains(local, "_shovel") && !contains(local, "_hoe"))
        return 0;
    if (contains(local, "netherite_"))
        return 2031;
    if (contains(local, "diamond_"))
        return 1561;
    if (contains(local, "iron_"))
        return 250;
    if (contains(local, "stone_"))
        return 131;
    if (contains(local, "golden_"))
        return 32;
    if (contains(local, "wooden_"))
        return 59;
    return 0;
}

void AutoMineMod::chargeToolUse() {
    void *p = player();
    if (!p)
        return;
    const int slot = world_.selected(p);
    if (slot < 0 || slot > 8)
        return;
    const auto slots = hotbarItems();
    const auto &it = slots[static_cast<std::size_t>(slot)];
    if (it.id.empty()) {
        toolIdAtSlot_[static_cast<std::size_t>(slot)].clear();
        return;
    }
    if (maxDurabilityOf(localPart(it.id)) == 0)
        return;
    if (toolIdAtSlot_[static_cast<std::size_t>(slot)] != it.id ||
        toolNetAtSlot_[static_cast<std::size_t>(slot)] != it.netId) {
        toolIdAtSlot_[static_cast<std::size_t>(slot)] = it.id;
        toolNetAtSlot_[static_cast<std::size_t>(slot)] = it.netId;
        toolUsedAtSlot_[static_cast<std::size_t>(slot)] = 0;
    }
    ++toolUsedAtSlot_[static_cast<std::size_t>(slot)];
}

int AutoMineMod::toolRemainingAt(int slot) { // -1: not a tracked tool
    const auto slots = hotbarItems();
    if (slot < 0 || slot > 8)
        return -1;
    const auto &it = slots[static_cast<std::size_t>(slot)];
    const int max = it.id.empty() ? 0 : maxDurabilityOf(localPart(it.id));
    if (!max)
        return -1;
    if (it.damage >= 0)
        return std::max(0, max - it.damage);
    if (toolIdAtSlot_[static_cast<std::size_t>(slot)] != it.id ||
        toolNetAtSlot_[static_cast<std::size_t>(slot)] != it.netId)
        return max; // untouched here
    return std::max(0, max - toolUsedAtSlot_[static_cast<std::size_t>(slot)]);
}

int AutoMineMod::bestPickaxeRemaining(int minTier) {
    int best = -1;
    const auto slots = hotbarItems();
    for (int slot = 0; slot < 9; ++slot) {
        const auto &it = slots[static_cast<std::size_t>(slot)];
        if (!it.count || it.id.empty())
            continue;
        int t{};
        if (classForItem(localPart(it.id), t) != ToolClass::Pickaxe || t < minTier)
            continue;
        best = std::max(best, toolRemainingAt(slot));
    }
    return best;
}

bool AutoMineMod::hasSparePickaxe(int minTier) {
    if (!craftReady_)
        return false;
    for (const auto &s : craft_.snapshot()) {
        if (!s.count || s.slot < 9)
            continue;
        int t{};
        if (classForItem(localPart(s.item), t) == ToolClass::Pickaxe && t >= minTier)
            return true;
    }
    return false;
}

AutoMineMod::ToolChoice AutoMineMod::chooseTool(const std::string &blockId) {
    ToolChoice best;
    int bestRemaining = -1;
    int bestQuality = -1;
    const auto need = classForBlock(blockId);
    const int minTier = requiredTier(blockId);
    void *p = player();
    if (!p)
        return best;
    if (need == ToolClass::None) {
        best.byHand = true;
        return best;
    }
    const auto slots = hotbarItems();
    for (int slot = 0; slot < 9; ++slot) {
        const auto &item = slots[static_cast<std::size_t>(slot)];
        if (item.count == 0 || item.id.empty())
            continue;
        int tier{};
        const auto kind = classForItem(item.id, tier);
        if (kind != need || tier < minTier)
            continue;
        const int rem = toolRemainingAt(slot);
        const int quality = automine::toolRank(item.id);
        if (automine::consumeToolFirst(quality, rem, slot, bestQuality, bestRemaining,
                                       best.slot, world_.selected(p))) {
            best.slot = slot;
            best.tier = tier;
            best.kind = kind;
            bestRemaining = rem;
            bestQuality = quality;
        }
    }
    if (best.slot < 0 && minTier == 0)
        best.byHand = true;
    return best;
}

bool AutoMineMod::ensureTool(const std::string &blockId) {
    const auto choice = chooseTool(blockId);
    if (classForBlock(blockId) != ToolClass::None) {
        const auto expected = hotbarItems();
        const auto actual = hotbarMemoryItems();
        for (int i = 0; i < 9; ++i) {
            int tier{};
            if (!expected[i].count ||
                classForItem(expected[i].id, tier) != classForBlock(blockId) ||
                tier < requiredTier(blockId))
                continue;
            if ((choice.slot < 0 || choice.slot == i) &&
                (!actual[i].available || actual[i].count == 0)) {
                if (toolSyncSlot_ < 0) {
                    toolSyncSlot_ = i;
                    toolSyncItem_ = expected[i].id;
                    toolSyncStage_ = toolSyncTicks_ = 0;
                    note("tool sync: native slot " + std::to_string(i) + " does not contain " +
                         toolSyncItem_ + "; mining paused");
                }
                return false;
            }
        }
    }
    if (choice.byHand) {
        const auto slots = hotbarMemoryItems();
        void *p = player();
        if (!p)
            return false;
        for (int i = 8; i >= 0; --i)
            if (slots[i].available && !slots[i].count) {
                if (world_.selected(p) != i)
                    return world_.select(p, static_cast<unsigned>(i));
                break;
            }
        return true;
    }
    if (choice.slot < 0)
        return false;
    void *p = player();
    if (!p)
        return false;
    if (world_.selected(p) != choice.slot) {
        if (!world_.select(p, static_cast<unsigned>(choice.slot)))
            return false;
        note("tool: selected slot " + std::to_string(choice.slot));
    }
    const auto actual = hotbarMemoryItems();
    int tier{};
    const auto expected = hotbarItems();
    const auto &held = expected[choice.slot];
    // Acknowledged forged results may retain the previous native item name.
    // The server owns the slot contents; don't throw away its result or craft
    // again merely because the native name has not caught up. A native handle
    // is still required before GameMode can act (see sendDeposit in the SDK).
    return world_.selected(p) == choice.slot && held.available && held.count > 0 &&
           actual[choice.slot].available && actual[choice.slot].count > 0 &&
           classForItem(held.id, tier) == choice.kind && tier >= requiredTier(blockId);
}

bool AutoMineMod::tickToolSync() {
    if (toolSyncSlot_ < 0)
        return false;
    if (anyScreenUp())
        return true;
    const auto actual = hotbarMemoryItems();
    const auto &slot = actual[toolSyncSlot_];
    if (slot.available && slot.count > 0 && localPart(slot.id) == localPart(toolSyncItem_)) {
        if (toolSyncStage_ > 0 && !craft_.backgroundClose()) {
            releaseInputs();
            return true;
        }
        note("tool sync: native item confirmed in slot " + std::to_string(toolSyncSlot_));
        toolSyncSlot_ = -1;
        hotbarCacheTick_ = -1;
        return false;
    }
    releaseInputs();
    if (!backgroundCraft_ || !craftReady_) {
        if (slot.available) {
            craft_.invalidateInventorySlot(toolSyncSlot_);
            note("tool sync: missing native tool; inventory entry invalidated");
            toolSyncSlot_ = -1;
            hotbarCacheTick_ = -1;
            return false;
        }
        return true;
    }
    if (craft_.status() == bedrock::CraftJobStatus::Running ||
        craft_.transferStatus() == bedrock::CraftJobStatus::Running)
        return true;
    ++toolSyncTicks_;
    if (toolSyncStage_ == 0) {
        if (craft_.backgroundActive()) {
            (void)craft_.backgroundClose();
            return true;
        }
        if (craft_.backgroundInventory()) {
            toolSyncStage_ = 1;
            toolSyncTicks_ = 0;
            note("tool sync: requesting background inventory refresh");
        }
    } else if (toolSyncStage_ == 1) {
        if (!craft_.backgroundActive() ||
            craft_.backgroundOpen(bedrock::NativeItemStackRequests::BackgroundKind::Inventory) ||
            craft_.backgroundFailed() || toolSyncTicks_ > 100) {
            if (craft_.backgroundClose()) {
                toolSyncStage_ = 2;
                toolSyncTicks_ = 0;
            }
        }
    } else if (toolSyncTicks_ >= 10 && slot.available) {
        const auto expected = hotbarItems();
        if (expected[toolSyncSlot_].count > 0 &&
            localPart(expected[toolSyncSlot_].id) == localPart(toolSyncItem_)) {
            if (toolSyncTicks_ == 200)
                note("tool sync: acknowledged result still awaits native inventory; not recrafting");
            return true;
        }
        craft_.invalidateInventorySlot(toolSyncSlot_);
        note("tool sync: tool missing/replaced in slot " + std::to_string(toolSyncSlot_) +
             "; choosing another tool or crafting a replacement");
        toolSyncSlot_ = -1;
        hotbarCacheTick_ = -1;
        return false;
    }
    if (toolSyncTicks_ == 200)
        note("tool sync: inventory unavailable; mining remains paused");
    return true;
}

std::unordered_map<std::string, int> AutoMineMod::visibleItems() {
    std::unordered_map<std::string, int> items;
    for (const auto &slot : hotbarItems())
        if (slot.count && !slot.id.empty())
            items[localPart(slot.id)] += slot.count;
    return items;
}

void AutoMineMod::enterNeedTool() {
    releaseInputs();
    phase_ = Phase::NeedTool;
    needTicks_ = 0;
    needTier_ = requiredTier(targetId_);
    if (toolSyncSlot_ >= 0) {
        wantPickaxeChain_ = classForBlock(targetId_) == ToolClass::Pickaxe;
        note("needtool: waiting for inventory synchronization");
        return;
    }
    if (classForBlock(targetId_) == ToolClass::Pickaxe) {
        if (needTier_ >= 4) {
            const bool diamondTier = needTier_ >= 5;
            const int mats = invCountOf(diamondTier ? "diamond" : "iron_ingot");
            const std::string tool = diamondTier ? "ダイヤのツルハシ" : "鉄のツルハシ";
            const std::string mat = diamondTier ? "ダイヤモンド" : "鉄インゴット";
            reply("この鉱石は" + tool + "以上じゃないと掘れないよ。" +
                  (mats >= 3 ? mat + "があるから作るね"
                             : mat + "が3個必要だよ（今" + std::to_string(mats) + "個）"));
            note("needtool: tier " + std::to_string(needTier_) + " for " + targetId_ + ", " + mat +
                 "=" + std::to_string(mats));
            wantPickaxeChain_ = true;
            tryStartPickaxeChain();
            return;
        }
        const auto plan = planPickaxe(visibleItems());
        reply("ツルハシがホットバーに無いよ。" + plan.summary);
        note("needtool: " + plan.summary + " possible=" + std::to_string(plan.possible));
        if (plan.possible) {
            wantPickaxeChain_ = true;
            tryStartPickaxeChain();
        }
    } else {
        reply("このブロック用のツールがホットバーに無いよ");
    }
}

void AutoMineMod::tickNeedTool(bedrock::Vec3 feet) {
    if (moveStage_ != MoveStage::None) {
        (void)ensureHotbarTick(moveItem_);
        return;
    }
    if (++needTicks_ % 20)
        return;
    if (needTicks_ % 200 == 20) {
        std::string snap;
        for (const auto &s : craft_.snapshot()) {
            if (!s.count)
                continue;
            snap += " " + std::to_string(s.slot) + ":" + localPart(s.item) + "x" +
                    std::to_string(s.count);
        }
        note("needtool diag: tier=" + std::to_string(needTier_) +
             " moveFailed=" + std::to_string(moveFailed_) +
             " moveStage=" + std::to_string(static_cast<int>(moveStage_)) +
             " wantChain=" + std::to_string(wantPickaxeChain_) +
             " chainActive=" + std::to_string(chainActive_) + " snap:" + snap);
    }
    if (chainActive_ && chainWaitingCobble_ && !hasPickaxeTier(1) && needTicks_ > 20 * 10) {
        chainFail("ツルハシが見つからない");
        return;
    }
    if (gatherActive_ && !chainActive_ && !hasPickaxeTier(1) && needTicks_ > 20 * 10) {
        if (hasTarget_)
            blacklist_.insert(target_);
        hasTarget_ = false;
        path_.clear();
        pathDig_.clear();
        pathBridge_.clear();
        pathParkour_.clear();
        pathPillar_.clear();
        pathIdx_ = 0;
        needTicks_ = 0;
        emptyScans_ = 6;
        releaseInputs();
        phase_ = Phase::Scan;
        note("needtool: gather path needs a tool I cannot make yet; relocating");
        return;
    }
    if (expeditionActive() && needTier_ >= 2) {
        const int surfY = lastSurfaceY_ == INT_MAX ? 70 : lastSurfaceY_;
        if (static_cast<int>(std::floor(feet.y)) < surfY - 8 && !hasPickaxeTier(needTier_) &&
            !hasSparePickaxe(needTier_)) {
            if (wantPickaxeChain_ && tryStartPickaxeChain())
                return; // chain path taken
            const int climb = surfY - static_cast<int>(std::floor(feet.y));
            if (totalSpareCount() >= climb) {
                enterAscend(surfY, "道具が作れないから、地上に戻って仕切り直すね",
                            "ascend: returning to surface y=" + std::to_string(surfY));
                return;
            }
        }
    }
    if (wantPickaxeChain_ && tickCount_ >= chainNextRetryTick_ && tryStartPickaxeChain())
        return;
    if (maybeStartExpedition())
        return;
    if (moveFailed_ && needTier_ > 0 && tickCount_ >= moveRetryTick_) {
        moveRetryTick_ = tickCount_ + 20 * 15;
        moveFailed_ = false;
        note("needtool: retrying the hotbar fetch");
    }
    if (needTier_ > 0 && !moveFailed_) {
        bool hotbarHas = false;
        std::string mainPick;
        int mainTier = -1;
        for (const auto &s : craft_.snapshot()) {
            if (!s.count)
                continue;
            int t{};
            if (classForItem(localPart(s.item), t) != ToolClass::Pickaxe || t < needTier_)
                continue;
            if (s.slot < 9)
                hotbarHas = true;
            else if (t > mainTier) {
                mainTier = t;
                mainPick = localPart(s.item);
            }
        }
        if (!hotbarHas && !mainPick.empty()) {
            (void)ensureHotbarTick(mainPick);
            return;
        }
    }
    if (ensureTool(targetId_)) {
        reply("ツールを確認。再開するね");
        phase_ = Phase::Scan;
        scanCursor_ = 0;
        bestDist2_ = 1e18;
        hasTarget_ = false;
    }
}

} // namespace automine::client
