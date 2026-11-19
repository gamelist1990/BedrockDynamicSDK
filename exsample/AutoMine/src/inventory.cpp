#include "auto_mine.hpp"

namespace automine::client {

std::array<AutoMineMod::InvSlot, 9> AutoMineMod::hotbarMemoryItems() {
    std::array<InvSlot, 9> out;
    if (craftReady_) {
        for (const auto &slot : craft_.memorySnapshot(true, 0, 9))
            if (slot.slot >= 0 && slot.slot < 9)
                out[slot.slot] = {slot.item, slot.count, true, slot.damage, slot.netId};
    } else if (void *p = player()) {
        for (int i = 0; i < 9; ++i)
            if (const auto item = access_.localInventoryItem(p, i))
                out[i] = {item->identifier, static_cast<int>(item->count), true};
    }
    return out;
}

std::array<AutoMineMod::InvSlot, 9> AutoMineMod::hotbarItems() {
    if (hotbarCacheTick_ == tickCount_)
        return hotbarCache_;
    auto out = hotbarMemoryItems();
    if (craftReady_) {
        for (const auto &slot : craft_.snapshot(true)) {
            if (slot.slot < 0 || slot.slot >= 9)
                continue;
            out[static_cast<std::size_t>(slot.slot)] = {slot.item, slot.count, true, slot.damage,
                                                      slot.netId};
        }
    }
    hotbarCache_ = out;
    hotbarCacheTick_ = tickCount_;
    return out;
}

std::vector<int> AutoMineMod::hotbarSlotCandidates(const std::string &local) {
    std::vector<int> out;
    auto addFrom = [&](const std::array<InvSlot, 9> &slots) {
        for (int i = 0; i < 9; ++i) {
            const auto &it = slots[static_cast<std::size_t>(i)];
            if (it.count && localPart(it.id) == local &&
                std::find(out.begin(), out.end(), i) == out.end())
                out.push_back(i);
        }
    };
    addFrom(hotbarItems());
    addFrom(hotbarMemoryItems());
    return out;
}

int AutoMineMod::placeSlotTick(const std::string &local, std::size_t &idx, int &ticks,
                               int fallback) {
    const auto cands = hotbarSlotCandidates(local);
    if (cands.empty())
        return fallback;
    if (idx >= cands.size())
        idx = 0;
    const int slot = cands[idx];
    if (++ticks > 40) {
        ticks = 0;
        idx = (idx + 1) % cands.size();
    }
    return slot;
}

void AutoMineMod::selectSlot(int slot) {
    if (slot < 0)
        return;
    void *p = player();
    if (p && world_.selected(p) != slot)
        world_.select(p, static_cast<unsigned>(slot));
}

bool AutoMineMod::isSpareBlock(const std::string &local) {
    return automine::buildingPriority(local) >= 0;
}

int AutoMineMod::buildingReserve(const std::string &id) {
    return 0; // wood is protected by material priority, not an unusable final reserve
}

std::array<AutoMineMod::InvSlot, 9> AutoMineMod::buildingHotbar() {
    auto actual = hotbarMemoryItems();
    const auto tracked = hotbarItems();
    for (int i = 0; i < 9; ++i) {
        if (!actual[i].available || !tracked[i].available ||
            localPart(actual[i].id) != localPart(tracked[i].id))
            actual[i].count = 0;
        else
            actual[i].count = std::min(actual[i].count, tracked[i].count);
    }
    return actual;
}

int AutoMineMod::spareCount() {
    int n = 0;
    for (const auto &it : buildingHotbar())
        if (isSpareBlock(localPart(it.id)))
            n += std::max(0, it.count - buildingReserve(localPart(it.id)));
    return n;
}

int AutoMineMod::totalSpareCount() {
    if (!craftReady_)
        return spareCount();
    int n = 0;
    for (const auto &s : craft_.snapshot())
        if (s.count && isSpareBlock(localPart(s.item)))
            n += std::max(0, s.count - buildingReserve(localPart(s.item)));
        else if (s.count && automine::rawBuildingWood(s.item))
            n += 4 * s.count; // potential supply, never a directly placeable stack
    return n;
}

int AutoMineMod::spareSlot() {
    const auto slots = buildingHotbar();
    int best = -1, bestCount = 0, priority = 3;
    for (int i = 0; i < 9; ++i) {
        const auto &it = slots[static_cast<std::size_t>(i)];
        const int available = it.count - buildingReserve(localPart(it.id));
        const int rank = automine::buildingPriority(it.id);
        if (available > 0 && rank >= 0 &&
            (rank < priority || (rank == priority && available > bestCount))) {
            best = i;
            bestCount = available;
            priority = rank;
        }
    }
    // A hotbar plank stack must not hide a disposable stack in the backpack.
    if (craftReady_)
        for (const auto &s : craft_.snapshot())
            if (s.count > 0 && s.slot >= 9 &&
                automine::buildingPriority(s.item) >= 0 &&
                automine::buildingPriority(s.item) < priority)
                return -1;
    return best;
}

int AutoMineMod::scaffoldSlot() {
    return spareSlot();
}

bool AutoMineMod::prepareBuildingSupply() {
    if (!craftReady_ || chainActive_)
        return chainActive_;
    if (anyScreenUp())
        return true;
    if (moveStage_ != MoveStage::None) {
        const bool ready = ensureHotbarTick(moveItem_);
        return ready || !moveFailed_;
    }
    const auto inventory = craft_.snapshot();
    std::string material;
    int priority = 3;
    for (const auto &s : inventory) {
        const int rank = automine::buildingPriority(s.item);
        if (s.count > 0 && rank >= 0 && rank < priority) {
            priority = rank;
            material = s.item;
        }
    }
    if (!material.empty()) {
        const bool ready = ensureHotbarTick(material);
        return ready || !moveFailed_;
    }
    if (tickCount_ < buildingCraftNext_ || !craft_.ready() || !craft_.recipesReady())
        return false;
    int logs{};
    const auto wood = chainWood(chainCounts(), logs);
    if (wood.empty() || logs < 1)
        return false;
    const std::string planks = "minecraft:" + wood + "_planks";
    bedrock::CraftRecipe recipe;
    if (!craft_.copyRecipe(planks, recipe))
        return false;
    chainSteps_ = {{ChainStep::Kind::OpenInventory},
                   {ChainStep::Kind::Craft, planks, planks, 1, false},
                   {ChainStep::Kind::CloseUi}};
    chainStep_ = 0;
    chainStepTicks_ = 0;
    chainJobStarted_ = false;
    chainRecoverTicks_ = -1;
    chainRetriedOpen_ = false;
    chainPendingFail_.clear();
    chainForPickaxe_ = chainForFurnace_ = chainForUtility_ = false;
    chainForBuilding_ = chainActive_ = true;
    chainReturn_ = phase_;
    phase_ = Phase::Craft;
    releaseInputs();
    note("building supply: converting raw wood to planks (last resort)");
    return true;
}

int AutoMineMod::scaffoldCount() { // hotbar + main inventory, via the snapshot
    return totalSpareCount();
}

int AutoMineMod::countAllItems(std::string_view key) {
    int total = 0;
    if (craftReady_)
        for (const auto &s : craft_.snapshot())
            if (contains(localPart(s.item), key))
                total += s.count;
    return total;
}

std::unordered_map<std::string, int> AutoMineMod::chainCounts() {
    std::unordered_map<std::string, int> have;
    if (craftReady_)
        for (const auto &s : craft_.snapshot())
            if (s.count > 0 && !s.item.empty())
                have[localPart(s.item)] += s.count;
    return have;
}

int AutoMineMod::countIn(const std::unordered_map<std::string, int> &items, std::string_view key) {
    int total = 0;
    for (const auto &[id, count] : items)
        if (contains(id, key))
            total += count;
    return total;
}

std::uint32_t AutoMineMod::invContainer(int) {
    return 12u;
}

int AutoMineMod::preferredToolSlot(const std::string &id) {
    return automine::toolSlot(id);
}

bool AutoMineMod::ensureHotbarTick(const std::string &rawId, int sourceSlot) {
    const auto localId = localPart(rawId);
    if (backgroundCraft_ && anyScreenUp())
        return false;
    if (!backgroundCraft_ && contains(topScreenName(), "chat"))
        return false;
    const long long stageTicks = tickCount_ - moveStageStart_;
    switch (moveStage_) {
    case MoveStage::None: {
        const int preferred = preferredToolSlot(localId);
        const auto snap = craft_.snapshot();
        bool occupied[36]{};
        for (const auto &s : snap)
            if (s.count > 0 && s.slot >= 0 && s.slot < 36)
                occupied[s.slot] = true;
        const auto hb = hotbarItems();
        for (int i = 0; i < 9 && i < static_cast<int>(hb.size()); ++i)
            if ((sourceSlot < 0 || sourceSlot == i) && hb[static_cast<std::size_t>(i)].count > 0 &&
                localPart(hb[static_cast<std::size_t>(i)].id) == localId)
                return true;
        int src = -1, srcCount = 0;
        std::int32_t srcNet = 0;
        std::string fullId;
        for (const auto &s : snap)
            if (s.count > 0 && s.slot >= 9 && (sourceSlot < 0 || s.slot == sourceSlot) &&
                localPart(s.item) == localId) {
                src = s.slot;
                srcCount = s.count;
                srcNet = s.netId;
                fullId = s.item;
                break;
            }
        if (src < 0)
            for (const auto &s : snap)
                if (s.count > 0 && s.slot >= 0 && s.slot < 9 && localPart(s.item) == localId &&
                    (preferred >= 0
                         ? s.slot != preferred
                         : !(s.slot < static_cast<int>(hb.size()) &&
                             hb[static_cast<std::size_t>(s.slot)].count > 0 &&
                             localPart(hb[static_cast<std::size_t>(s.slot)].id) == localId))) {
                    src = s.slot;
                    srcCount = s.count;
                    srcNet = s.netId;
                    fullId = s.item;
                    break;
                }
        if (src < 0) {
            moveFailed_ = true;
            return false;
        }
        moveItem_ = fullId;
        moveSrc_ = src;
        moveSrcNet_ = srcNet;
        moveCount_ = srcCount;
        moveNeedEvict_ = false;
        moveFailPending_ = false;
        if (!moveReplan_)
            moveAttempts_ = 0; // retries keep their budget
        moveReplan_ = false;
        moveEvictDone_ = false;
        int dst = -1;
        for (int i = 4; i < 9; ++i)
            if (!occupied[i]) {
                dst = i;
                break;
            }
        if (dst < 0)
            for (int i = 0; i < 4; ++i)
                if (!occupied[i]) {
                    dst = i;
                    break;
                }
        if (preferred >= 0)
            dst = occupied[preferred] ? -1 : preferred;
        if (dst >= 0) {
            moveDst_ = dst;
            note("hotbar move: " + localId + " from main slot " + std::to_string(src) +
                 " to hotbar " + std::to_string(dst));
        } else {
            // Hotbar full: evict the least valuable non-tool stack
            // to a free main-inventory slot first.
            int evict = -1, evictScore = -1, evictCount = 0;
            std::int32_t evictNet = 0;
            std::string evictId;
            for (const auto &s : snap) {
                if (s.count <= 0 || s.slot >= 9)
                    continue;
                if (preferred >= 0 && s.slot != preferred)
                    continue;
                const std::string id = localPart(s.item);
                if ((id == localId && sourceSlot < 0) ||
                    (preferred < 0 && (id == "crafting_table" || id == "furnace")))
                    continue;
                if (preferred < 0 && (contains(id, "sword") || id == "bow"))
                    continue;
                int t{};
                if (preferred < 0 && classForItem(id, t) != ToolClass::None)
                    continue;
                int score = 10;
                if (contains(id, "sapling"))
                    score = 100;
                else if (contains(id, "seed"))
                    score = 90;
                else if (id == "apple")
                    score = 80;
                else if (contains(id, "sand"))
                    score = 70;
                else if (contains(id, "dirt"))
                    score = 60;
                else if (contains(id, "gravel"))
                    score = 50;
                else if (contains(id, "flower"))
                    score = 40;
                if (score > evictScore) {
                    evictScore = score;
                    evict = s.slot;
                    evictCount = s.count;
                    evictNet = s.netId;
                    evictId = s.item;
                }
            }
            int freeMain = -1;
            for (int i = 9; i < 36; ++i)
                if (!occupied[i]) {
                    freeMain = i;
                    break;
                }
            if (evict < 0 || freeMain < 0) {
                moveFailed_ = true;
                return false;
            }
            moveDst_ = evict;
            moveEvict_ = evict;
            moveEvictCount_ = evictCount;
            moveEvictNet_ = evictNet;
            moveEvictId_ = evictId;
            moveFreeMain_ = freeMain;
            moveNeedEvict_ = true;
            note("hotbar move: evicting " + localPart(evictId) + " from hotbar " +
                 std::to_string(evict) + " for " + localId);
        }
        moveStage_ = MoveStage::OpenInv;
        moveStageStart_ = tickCount_;
        return false;
    }
    case MoveStage::OpenInv: {
        if (backgroundCraft_) {
            if (!craft_.backgroundInventory() || craft_.backgroundFailed()) {
                if (stageTicks > 120) {
                    moveFailed_ = true;
                    moveStage_ = MoveStage::CloseUi;
                    moveStageStart_ = tickCount_;
                }
                return false;
            }
            if (!craft_.backgroundOpen(bedrock::NativeItemStackRequests::BackgroundKind::Inventory))
                return false;
        }
        if (!backgroundCraft_ && screenReady_) {
            const std::string screen = topScreenName();
            if (contains(screen, "inventory")) {
                if (moveScreenOkTicks_ < 0)
                    moveScreenOkTicks_ = 0;
                if (++moveScreenOkTicks_ < 10)
                    return false;
                const bool evicting = moveNeedEvict_ && !moveEvictDone_;
                const bool ok =
                    evicting ? craft_.startTransfer(invContainer(moveEvict_), moveEvict_,
                                                    moveEvictNet_, invContainer(moveFreeMain_),
                                                    moveFreeMain_, moveEvictCount_, moveEvictId_)
                             : craft_.startTransfer(invContainer(moveSrc_), moveSrc_, moveSrcNet_,
                                                    invContainer(moveDst_), moveDst_, moveCount_,
                                                    moveItem_);
                if (ok) {
                    moveStage_ = evicting ? MoveStage::Evict : MoveStage::Bring;
                    moveDeadline_ = tickCount_ + 20 * 15;
                    moveScreenOkTicks_ = -1;
                    return false;
                }
                if (stageTicks > 400) {
                    note("hotbar move: transfer never dispatched");
                    moveFailed_ = true;
                    moveStage_ = MoveStage::None;
                }
                return false;
            }
            moveScreenOkTicks_ = -1;
            if (!screen.empty() && screen != "hud_screen") {
                if ((stageTicks % 45) == 1)
                    pressKey(0x1B, true);
                if ((stageTicks % 45) == 3)
                    pressKey(0x1B, false);
            } else {
                if ((stageTicks % 45) == 1)
                    pressKey(0x45, true); // VK_E
                if ((stageTicks % 45) == 3)
                    pressKey(0x45, false);
            }
            if (stageTicks > 400) {
                note("hotbar move: inventory screen did not open (top: " + screen + ")");
                moveFailed_ = true;
                moveStage_ = MoveStage::None;
            }
            return false;
        }
        if (stageTicks >= 25) {
            const bool evicting = moveNeedEvict_ && !moveEvictDone_;
            const bool ok =
                evicting
                    ? craft_.startTransfer(invContainer(moveEvict_), moveEvict_, moveEvictNet_,
                                           invContainer(moveFreeMain_), moveFreeMain_,
                                           moveEvictCount_, moveEvictId_)
                    : craft_.startTransfer(invContainer(moveSrc_), moveSrc_, moveSrcNet_,
                                           invContainer(moveDst_), moveDst_, moveCount_, moveItem_);
            if (ok) {
                moveStage_ = evicting ? MoveStage::Evict : MoveStage::Bring;
                moveDeadline_ = tickCount_ + 20 * 15;
                return false;
            }
            // Adapter gate busy (an earlier transfer is still
            // resolving): keep waiting rather than failing.
            if (stageTicks > 300) {
                note("hotbar move: transfer never dispatched");
                moveFailed_ = true;
                moveStage_ = MoveStage::None;
            }
            return false;
        }
        if (!backgroundCraft_ && moveAttempts_ == 0 && (stageTicks % 45) == 1)
            pressKey(0x45, true); // VK_E
        if (!backgroundCraft_ && moveAttempts_ == 0 && (stageTicks % 45) == 3)
            pressKey(0x45, false);
        if (stageTicks > 300) {
            note("hotbar move: inventory screen did not open");
            moveFailed_ = true;
            moveStage_ = MoveStage::None;
        }
        return false;
    }
    case MoveStage::Evict:
    case MoveStage::Bring: {
        if (tickCount_ <= moveDeadline_) {
            const auto st = craft_.transferStatus();
            if (st == bedrock::CraftJobStatus::Running)
                return false;
            craft_.clearTransferStatus();
            if (st != bedrock::CraftJobStatus::Failed) {
                if (moveStage_ == MoveStage::Evict) {
                    moveEvictDone_ = true;
                    if (craft_.startTransfer(invContainer(moveSrc_), moveSrc_, moveSrcNet_,
                                             invContainer(moveDst_), moveDst_, moveCount_,
                                             moveItem_)) {
                        moveStage_ = MoveStage::Bring;
                        moveDeadline_ = tickCount_ + 20 * 15;
                        return false;
                    }
                } else {
                    moveStage_ = MoveStage::CloseUi;
                    moveStageStart_ = tickCount_;
                    return false;
                }
            }
            note("hotbar move: transfer failed");
        } else {
            note("hotbar move: transfer timed out");
        }
        if (moveAttempts_ < 2) {
            ++moveAttempts_;
            note("hotbar move: retrying with a fresh plan");
            moveStage_ = MoveStage::None;
            moveReplan_ = true;
            moveStageStart_ = tickCount_;
            return false;
        }
        moveFailPending_ = true;
        moveStage_ = MoveStage::CloseUi;
        moveStageStart_ = tickCount_;
        return false;
    }
    case MoveStage::CloseUi: {
        if (backgroundCraft_) {
            if (!craft_.backgroundClose())
                return false;
            moveStage_ = MoveStage::None;
            if (moveFailPending_) {
                moveFailPending_ = false;
                moveFailed_ = true;
                return false;
            }
            note("hotbar move: background done");
            return true;
        }
        // Verified close when the screen name is readable: the
        // gameplay-active event alone has lied in both directions.
        bool closed = game_.gameplayActive();
        if (screenReady_) {
            const std::string screen = topScreenName();
            closed = screen.empty() || screen == "hud_screen";
        }
        if (closed || stageTicks > 70) {
            moveStage_ = MoveStage::None;
            if (moveFailPending_) {
                moveFailPending_ = false;
                moveFailed_ = true;
                return false;
            }
            note("hotbar move: done");
            return true;
        }
        if (stageTicks == 1 || stageTicks == 30)
            pressKey(0x1B, true);
        if (stageTicks == 3 || stageTicks == 32)
            pressKey(0x1B, false);
        return false;
    }
    }
    return false;
}

bool AutoMineMod::tickInventoryMaintenance(bedrock::Vec3 feet) {
    if (!craftReady_ || !craft_.ready())
        return false;
    if (maintenanceMove_) {
        releaseInputs();
        (void)ensureHotbarTick(maintenanceItem_);
        if (moveStage_ == MoveStage::None) {
            maintenanceMove_ = false;
            maintenanceNext_ = tickCount_ + (moveFailed_ ? 200 : 20);
        }
        return true;
    }
    if (maintenanceDrop_) {
        if (craft_.transferStatus() == bedrock::CraftJobStatus::Running)
            return true;
        craft_.clearTransferStatus();
        if (!craft_.backgroundClose())
            return true;
        maintenanceDrop_ = false;
        maintenanceNext_ = tickCount_ + 60;
        return true;
    }
    if ((phase_ != Phase::Scan && phase_ != Phase::Navigate) || chainActive_ ||
        moveStage_ != MoveStage::None || craft_.status() == bedrock::CraftJobStatus::Running ||
        craft_.transferStatus() == bedrock::CraftJobStatus::Running || inWaterNow(feet) ||
        tickCount_ < maintenanceNext_ || automationScreenUp())
        return false;
    maintenanceNext_ = tickCount_ + 40;
    const auto inventory = craft_.snapshot();
    std::array<std::string, 4> best{};
    std::array<int, 4> rank{}, remaining{};
    std::array<int, 4> source{-1, -1, -1, -1};
    for (const auto &item : inventory) {
        const int dst = preferredToolSlot(item.item), r = automine::toolRank(item.item);
        const int rem = std::max(0, maxDurabilityOf(localPart(item.item)) -
                                   std::max(0, item.damage));
        if (dst >= 0 && item.count > 0 &&
            automine::consumeToolFirst(r, rem, item.slot, rank[dst], remaining[dst],
                                       source[dst], -1)) {
            rank[dst] = r;
            remaining[dst] = rem;
            source[dst] = item.slot;
            best[dst] = localPart(item.item);
        }
    }
    for (int dst = 0; dst < 4; ++dst)
        if (!best[dst].empty() && source[dst] >= 9) {
            maintenanceMove_ = true;
            maintenanceItem_ = best[dst];
            moveFailed_ = false;
            releaseInputs();
            (void)ensureHotbarTick(maintenanceItem_, source[dst]);
            return true;
        }
    if (tryStartUtilityChain())
        return true;
    if (!spareCount())
        for (const auto &item : inventory)
            if (item.slot >= 9 && isSpareBlock(localPart(item.item)) &&
                item.count > buildingReserve(localPart(item.item))) {
                maintenanceMove_ = true;
                maintenanceItem_ = localPart(item.item);
                moveFailed_ = false;
                releaseInputs();
                (void)ensureHotbarTick(maintenanceItem_);
                return true;
            }
    if (autoDiscard_ && backgroundCraft_) {
        const std::set<std::string> keep{localPart(targetId_), localPart(scanSavedId_),
                                         localPart(dropOf(targetId_)), localPart(goalItem_)};
        for (const auto &item : inventory)
            if (automine::discardable(item.item, item.slot, discardItems_, keep)) {
                if (!craft_.backgroundInventory())
                    return false;
                if (!craft_.backgroundOpen(
                        bedrock::NativeItemStackRequests::BackgroundKind::Inventory)) {
                    maintenanceNext_ = tickCount_ + 1;
                    return true;
                }
                if (craft_.startDrop(item.slot, item.netId, item.count, item.item)) {
                    maintenanceDrop_ = true;
                    releaseInputs();
                    return true;
                }
                (void)craft_.backgroundClose();
                maintenanceNext_ = tickCount_ + 200;
                break;
            }
    }
    return false;
}

int AutoMineMod::findHotbarSlot(const std::string &localId) {
    const auto slots = hotbarItems();
    for (int i = 0; i < 9 && i < static_cast<int>(slots.size()); ++i)
        if (slots[static_cast<std::size_t>(i)].count > 0 &&
            localPart(slots[static_cast<std::size_t>(i)].id) == localId)
            return i;
    return -1;
}

std::optional<bedrock::CraftInventorySlot> AutoMineMod::findInvItem(const std::string &localId) {
    for (const auto &s : craft_.snapshot())
        if (s.count > 0 && localPart(s.item) == localId)
            return s;
    return std::nullopt;
}

std::optional<bedrock::CraftInventorySlot>
AutoMineMod::findInvItemPred(const std::function<bool(const std::string &)> &pred) {
    for (const auto &s : craft_.snapshot())
        if (s.count > 0 && pred(localPart(s.item)))
            return s;
    return std::nullopt;
}

} // namespace automine::client
