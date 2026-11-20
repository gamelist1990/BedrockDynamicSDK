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
    const auto need = classForBlock(blockId);

    // No tool is required: never enter tool/native inventory synchronization.
    // This is the bare-hand path and is especially important for tier-0 blocks.
    if (need == ToolClass::None || requiredTier(blockId) == 0 && choice.byHand) {
        digHandAllowed_ = true;
        toolSyncSlot_ = -1;
        toolSyncTicks_ = toolSyncStage_ = 0;
        toolSyncItem_.clear();
        hotbarMoveAttempts_ = 0;
        hotbarMoveUnavailableItem_.clear();
        hotbarMoveTarget_ = {INT_MIN, 0, 0};
        return true;
    }

    if (need != ToolClass::None) {
        const int minTier = requiredTier(blockId);
        // Native inventory is authoritative.  Search all 36 inventory slots before
        // trusting the hotbar shadow: a craft response can deposit into a different slot.
        if (craftReady_) {
            int bestNativeSlot = 36, bestNativeTier = -1, bestNativeRank = -1;
            std::string bestNativeId;
            for (const auto &native : craft_.memorySnapshot(true, 0, 36)) {
                if (!native.count || native.item.empty())
                    continue;
                // Native inventory is indices 0..35.  Empty names and out-of-range
                // entries must never be interpreted as matching tools.
                if (native.slot < 0 || native.slot >= 36)
                    continue;
                int tier{};
                const std::string id = localPart(native.item);
                if (classForItem(id, tier) != need || tier < minTier)
                    continue;
                const int rank = automine::toolRank(id);
                if (rank > bestNativeRank ||
                    (rank == bestNativeRank && native.slot < bestNativeSlot)) {
                    bestNativeRank = rank;
                    bestNativeTier = tier;
                    bestNativeSlot = native.slot;
                    bestNativeId = id;
                }
            }
            if (bestNativeSlot >= 9 && bestNativeSlot < 36) {
                if (hasTarget_ && hotbarMoveTarget_ == target_) {
                    ++hotbarMoveAttempts_;
                } else {
                    hotbarMoveTarget_ = hasTarget_ ? target_ : BlockPos3{INT_MIN, 0, 0};
                    hotbarMoveAttempts_ = 1;
                }
                if (hotbarMoveAttempts_ > 3) {
                    hotbarMoveUnavailableItem_ = bestNativeId;
                    moveFailed_ = true;
                    note("hotbar move: tool unavailable after 3 target retries; fallback from " +
                         bestNativeId + " for " + localPart(blockId));
                    digHandAllowed_ = minTier == 0;
                    if (digHandAllowed_) {
                        toolSyncSlot_ = -1;
                        toolSyncTicks_ = toolSyncStage_ = 0;
                        toolSyncItem_.clear();
                        return true;
                    }
                    return false;
                }
                note("tool sync: native inventory search found " + bestNativeId +
                     " in main slot " + std::to_string(bestNativeSlot) +
                     "; moving to hotbar attempt=" + std::to_string(hotbarMoveAttempts_) + "/3");
                (void)ensureHotbarTick(bestNativeId, bestNativeSlot);
                return false;
            }
            if (bestNativeSlot >= 0 && bestNativeSlot < 9) {
                void *p = player();
                if (p && world_.selected(p) != bestNativeSlot)
                    world_.select(p, static_cast<unsigned>(bestNativeSlot));
                hotbarCacheTick_ = -1;
                hotbarMoveAttempts_ = 0;
                hotbarMoveUnavailableItem_.clear();
                note("tool sync: native inventory search found " + bestNativeId +
                     " in slot " + std::to_string(bestNativeSlot) +
                     "; binding updated");
                toolSyncSlot_ = -1;
                toolSyncStage_ = toolSyncTicks_ = 0;
                toolSyncItem_.clear();
                return p && world_.selected(p) == bestNativeSlot;
            }
        }

        const auto expected = hotbarItems();
        const auto actual = hotbarMemoryItems();
        for (int i = 0; i < 9; ++i) {
            int tier{};
            if (!expected[i].count ||
                classForItem(expected[i].id, tier) != need || tier < minTier)
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

        // Soft blocks may use the best available alternative tool, or the bare hand.
        const std::string softBlock = localPart(blockId);
        if (minTier == 0 && (softBlock == "grass_block" || softBlock == "dirt" ||
                             contains(softBlock, "dirt") || softBlock == "podzol" ||
                             softBlock == "mycelium" || softBlock == "sand" ||
                             softBlock == "gravel" || softBlock == "clay")) {
            int altSlot = -1, altRank = -1;
            const auto native = craftReady_ ? craft_.memorySnapshot(true, 0, 36)
                                           : std::vector<bedrock::CraftInventorySlot>{};
            for (const auto &it : native) {
                if (!it.count || it.item.empty()) continue;
                if (it.slot < 0 || it.slot >= 36) continue;
                int tier{};
                if (classForItem(it.item, tier) != ToolClass::None && it.slot < 9) {
                    const int rank = automine::toolRank(it.item);
                    if (rank > altRank) { altRank = rank; altSlot = it.slot; }
                }
            }
            if (altSlot >= 0) {
                void *p = player();
                if (p && world_.selected(p) != altSlot)
                    world_.select(p, static_cast<unsigned>(altSlot));
                note("tool sync: soft-block alternative selected slot " +
                     std::to_string(altSlot) + " (best native tool)");
                return p && world_.selected(p) == altSlot;
            }
            digHandAllowed_ = true;
            note("tool sync: soft-block fallback is bare hand");
            return true;
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

    ++toolSyncTicks_;
    if (toolSyncTicks_ % 20 == 0)
        note("tool sync: waiting for inventory synchronization tick=" +
             std::to_string(toolSyncTicks_) + " stage=" + std::to_string(toolSyncStage_) +
             " target=" + localPart(toolSyncItem_));

    // Authoritative native whole-inventory search.  Rebind to any matching slot,
    // in hotbar or backpack, instead of checking only the stale expected slot.
    if (craftReady_) {
        for (const auto &native : craft_.memorySnapshot(true, 0, 36)) {
            if (!native.count || native.item.empty())
                continue;
            if (native.slot < 0 || native.slot >= 36)
                continue;
            if (toolSyncItem_.empty())
                continue;
            if (localPart(native.item) != localPart(toolSyncItem_))
                continue;
            if (native.slot < 9) {
                void *p = player();
                if (p)
                    (void)world_.select(p, static_cast<unsigned>(native.slot));
                note("tool sync: whole-inventory search found " + localPart(native.item) +
                     " in native slot " + std::to_string(native.slot) +
                     "; binding updated");
                if (toolSyncStage_ > 0)
                    (void)craft_.backgroundClose();
                craft_.invalidateInventorySlot(native.slot);
                toolSyncSlot_ = -1;
                toolSyncStage_ = toolSyncTicks_ = 0;
                toolSyncItem_.clear();
                hotbarCacheTick_ = -1;
                return false;
            }
            note("tool sync: whole-inventory search found " + localPart(native.item) +
                 " in main slot " + std::to_string(native.slot) +
                 "; moving to hotbar");
            (void)ensureHotbarTick(localPart(native.item), native.slot);
            return true;
        }
    }

    releaseInputs();
    if (!backgroundCraft_ || !craftReady_) {
        if (toolSyncTicks_ >= 100) {
            craft_.invalidateInventorySlot(toolSyncSlot_);
            note("tool sync: native item absent after " + std::to_string(toolSyncTicks_) +
                 " ticks; inventory entry invalidated");
            toolSyncSlot_ = -1;
            toolSyncStage_ = toolSyncTicks_ = 0;
            toolSyncItem_.clear();
            hotbarCacheTick_ = -1;
        }
        return true;
    }

    if (craft_.status() == bedrock::CraftJobStatus::Running ||
        craft_.transferStatus() == bedrock::CraftJobStatus::Running)
        return true;

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
                note("tool sync: refresh response complete; native whole-inventory search");
            }
        }
    } else if (toolSyncStage_ == 2) {
        // Stage 2 is intentionally finite: refresh, search, recraft, fallback/skip.
        if (toolSyncTicks_ >= 40) {
            bool absent = true;
            for (const auto &native : craft_.memorySnapshot(true, 0, 36))
                if (native.count && !native.item.empty() && native.slot >= 0 && native.slot < 36 &&
                    !toolSyncItem_.empty() &&
                    localPart(native.item) == localPart(toolSyncItem_)) {
                    absent = false;
                    break;
                }
            if (absent) {
                note("tool sync: item confirmed absent by native whole-inventory search");
                if (tryStartUtilityChain()) {
                    note("tool sync: materials available; restarting finite replacement craft " +
                         localPart(toolSyncItem_));
                    toolSyncSlot_ = -1;
                    toolSyncStage_ = toolSyncTicks_ = 0;
                    toolSyncItem_.clear();
                    hotbarCacheTick_ = -1;
                    return true;
                }

                const std::string block = localPart(targetId_);
                const bool soft = block == "grass_block" || block == "dirt" ||
                                  contains(block, "dirt") || block == "podzol" ||
                                  block == "mycelium" || block == "sand" ||
                                  block == "gravel" || block == "clay";
                if (soft) {
                    digHandAllowed_ = true;
                    note("tool sync: materials unavailable; soft-block fallback is bare hand " +
                         block);
                    toolSyncSlot_ = -1;
                    toolSyncStage_ = toolSyncTicks_ = 0;
                    toolSyncItem_.clear();
                    hotbarCacheTick_ = -1;
                    return false;
                }

                if (hasTarget_) {
                    blacklist_.insert(target_);
                    note("tool sync: no replacement materials/tool and block not soft; skipping target " +
                         targetId_ + " reason=required_tool_absent");
                    hasTarget_ = false;
                } else {
                    note("tool sync: no replacement and no soft fallback; target skip reason=required_tool_absent");
                }
                phase_ = Phase::Scan;
                scanCursor_ = 0;
                bestDist2_ = 1e18;
                emptyScans_ = 0;
                path_.clear();
                pathDig_.clear();
                pathBridge_.clear();
                pathParkour_.clear();
                pathPillar_.clear();
                toolSyncSlot_ = -1;
                toolSyncStage_ = toolSyncTicks_ = 0;
                toolSyncItem_.clear();
                hotbarCacheTick_ = -1;
                return false;
            }
        }
    }

    if (toolSyncTicks_ >= 240) {
        note("tool sync: refresh/whole-search exhausted at 240 ticks; escalating replacement/fallback");
        toolSyncTicks_ = 200;
        toolSyncStage_ = 2;
    }
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
