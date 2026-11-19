#include "auto_mine.hpp"

namespace automine::client {

std::string AutoMineMod::smeltYieldOf(const std::string &item) {
    if (item == "raw_iron")
        return "iron_ingot";
    if (item == "raw_gold")
        return "gold_ingot";
    if (item == "raw_copper")
        return "copper_ingot";
    const std::string cooked = cookedMeatOf(item);
    if (!cooked.empty())
        return cooked;
    return item;
}

void AutoMineMod::startSmelt(const std::string &item, int count) {
    smeltItem_ = item;
    smeltCount_ = count;
    smeltYieldItem_ = smeltYieldOf(item);
    smeltYieldBase_ = countAllItems(smeltYieldItem_);
    smeltStage_ = SmeltStage::CraftFurnace;
    smeltTicks_ = 0;
    smeltPlaced_ = false;
    smeltPlaceCell_ = {INT_MIN, 0, 0};
    smeltAimedTick_ = -1;
    smeltPocketing_ = false;
    smeltPocketDug_ = false;
    smeltPocketBad_ = 0;
    smeltPocket_ = {INT_MIN, 0, 0};
    smeltPlaceCells_.clear();
    smeltPlaceIdx_ = 0;
    smeltCellTicks_ = 0;
    smeltTransferStarted_ = false;
    smeltUseAltCont_ = false;
    smeltMoved_ = 0;
    smeltFuelBurnTicks_ = 0;
    smeltFuelPendingBurn_ = 0;
    smeltFuelBurnPer_ = 0;
    smeltExpect_ = count;
    smeltFurnaceGhost_ = false;
    smeltFailReason_.clear();
    furnaceChainTried_ = false;
    smeltCollectTries_ = 0;
    smeltBreakRetries_ = 0;
    smeltDone_ = false;
    smeltFailed_ = false;
    phase_ = Phase::Smelt;
    note("smelt: start " + item + " x" + std::to_string(count));
}

void AutoMineMod::smeltAdvance() {
    smeltStage_ = static_cast<SmeltStage>(static_cast<int>(smeltStage_) + 1);
    smeltTicks_ = 0;
    smeltAimedTick_ = -1;
    smeltTransferStarted_ = false;
    smeltUseAltCont_ = false;
    smeltPlaceArmed_ = false;
    smeltNoSlotRetries_ = 0;
    smeltNoSlotTicks_ = 0;
    smeltPlaceSlotIdx_ = 0;
    smeltPlaceSlotTicks_ = 0;
    setUse(false);
    pressKey(kKeyS, false);
}

void AutoMineMod::smeltFail(const std::string &why) {
    smeltFailReason_ = why;
    note("smelt failed: " + why);
    reply("精錬に失敗したよ（" + why + "）");
    setUse(false);
    setAttack(false);
    releaseInputs();
    smeltFailed_ = true;
    smeltStage_ = SmeltStage::Failed;
    phase_ = Phase::Idle;
}

bool AutoMineMod::tryStartFurnaceChain() {
    if (!craftReady_ || !craft_.recipesReady() || !craft_.ready() || chainActive_)
        return false;
    if (craft_.status() == bedrock::CraftJobStatus::Running)
        return false;
    craft_.clearStatus();
    const auto have = chainCounts();
    if (countIn(have, "cobblestone") < 8 && countIn(have, "cobbled_deepslate") < 8 &&
        countIn(have, "blackstone") < 8)
        return false;
    std::vector<ChainStep> steps;
    const bool localTable = placedTableExists();
    if (!localTable) {
        if (countIn(have, "crafting_table") >= 1) {
            steps.push_back(ChainStep{ChainStep::Kind::PlaceTable});
        } else {
            const int planks = countIn(have, "planks");
            steps.push_back(ChainStep{ChainStep::Kind::OpenInventory});
            if (planks < 4) {
                int logCount = 0;
                const std::string wood = chainWood(have, logCount);
                const int logCrafts = (4 - planks + 3) / 4;
                if (wood.empty() || logCrafts > logCount)
                    return false;
                steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:" + wood + "_planks",
                                          "minecraft:" + wood + "_planks", logCrafts, false});
            }
            steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:WorkBench_recipeId",
                                      "minecraft:crafting_table", 1, false});
            steps.push_back(ChainStep{ChainStep::Kind::CloseUi});
            steps.push_back(ChainStep{ChainStep::Kind::PlaceTable});
        }
    }
    steps.push_back(ChainStep{ChainStep::Kind::OpenTable});
    steps.push_back(
        ChainStep{ChainStep::Kind::Craft, "minecraft:furnace", "minecraft:furnace", 1, true});
    steps.push_back(ChainStep{ChainStep::Kind::CloseUi});
    chainSteps_ = std::move(steps);
    chainStep_ = 0;
    chainActive_ = true;
    chainJobStarted_ = false;
    chainPlaceCell_ = {INT_MIN, 0, 0};
    chainPlaceCells_.clear();
    chainPlaceIdx_ = 0;
    placeCellTicks_ = 0;
    chainPocketing_ = false;
    chainPocketDug_ = false;
    chainPocketBad_ = 0;
    chainPocket_ = {INT_MIN, 0, 0};
    chainStepTicks_ = 0;
    chainAimedTick_ = -1;
    chainRetriedOpen_ = false;
    chainRecoverTicks_ = -1;
    chainPendingFail_.clear();
    chainWaitingCobble_ = false;
    chainForPickaxe_ = false;
    chainForFurnace_ = true;
    chainReturn_ = Phase::Smelt;
    phase_ = Phase::Craft;
    note("smelt: furnace craft chain (" + std::to_string(chainSteps_.size()) + " steps)");
    return true;
}

bool AutoMineMod::tickSmeltTransfer(std::uint32_t dstContainer, int dstSlot, bool isFuel) {
    if (!smeltTransferStarted_) {
        std::optional<bedrock::CraftInventorySlot> src;
        int want = 0;
        if (isFuel) {
            const int needTicks = smeltMoved_ * 200 - smeltFuelBurnTicks_;
            if (needTicks <= 0) {
                note("smelt: fuel loaded");
                smeltAdvance();
                return true;
            }
            src = findInvItem("coal");
            smeltFuelBurnPer_ = 1600;
            if (!src) {
                src = findInvItemPred([](const std::string &id) {
                    return id.size() > 7 && id.compare(id.size() - 7, 7, "_planks") == 0;
                });
                smeltFuelBurnPer_ = 300;
            }
            if (!src) {
                src = findInvItemPred([](const std::string &id) {
                    return id.size() > 4 && id.compare(id.size() - 4, 4, "_log") == 0;
                });
                smeltFuelBurnPer_ = 300;
            }
            if (!src) {
                if (smeltFuelBurnTicks_ > 0) {
                    smeltExpect_ = std::min(smeltMoved_, smeltFuelBurnTicks_ / 200);
                    note("smelt: fuel short, cooking " + std::to_string(smeltExpect_) + " of " +
                         std::to_string(smeltMoved_));
                    smeltAdvance();
                    return true;
                }
                smeltFail("燃料が無い");
                return true;
            }
            want = (needTicks + smeltFuelBurnPer_ - 1) / smeltFuelBurnPer_;
        } else {
            src = findInvItem(smeltItem_);
            if (!src) {
                smeltFail("精錬する材料が無い");
                return true;
            }
            want = smeltCount_;
        }
        const int n = std::min(want, src->count);
        if (!isFuel)
            smeltMoved_ = n;
        else
            smeltFuelPendingBurn_ = n * smeltFuelBurnPer_;
        const std::uint32_t cont = smeltUseAltCont_ ? 12u : (src->slot < 9 ? 28u : 29u);
        if (!craft_.startTransfer(cont, src->slot, src->netId, dstContainer, dstSlot, n, src->item))
            return false; // adapter busy; retry next tick
        smeltTransferStarted_ = true;
        return false;
    }
    const auto st = craft_.transferStatus();
    if (st == bedrock::CraftJobStatus::Running)
        return false;
    if (st == bedrock::CraftJobStatus::Done) {
        craft_.clearTransferStatus();
        if (isFuel) {
            smeltFuelBurnTicks_ += smeltFuelPendingBurn_;
            smeltFuelPendingBurn_ = 0;
            if (smeltFuelBurnTicks_ < smeltMoved_ * 200) {
                // More burn needed: loop the stage for another
                // stack instead of advancing.
                smeltTransferStarted_ = false;
                note("smelt: fuel topped up (burn " + std::to_string(smeltFuelBurnTicks_) +
                     " ticks)");
                return false;
            }
        } else {
            // The yield target follows what was really loaded.
            smeltExpect_ = smeltMoved_;
        }
        note(std::string("smelt: ") + (isFuel ? "fuel" : "input") + " loaded");
        smeltAdvance();
        return true;
    }
    // Failed: the screen-open inventory may be addressed as the plain
    // container 12 instead of 28/29 — retry once with the other form.
    craft_.clearTransferStatus();
    if (!smeltUseAltCont_) {
        smeltUseAltCont_ = true;
        smeltTransferStarted_ = false;
        note("smelt: transfer failed, retrying with container 12");
        return false;
    }
    smeltFail("アイテム移動に失敗");
    return true;
}

void AutoMineMod::tickSmelt(bedrock::Vec3 feet) {
    if ((backgroundCraft_ && anyScreenUp()) || contains(topScreenName(), "chat")) {
        releaseInputs();
        return;
    }
    if (backgroundCraft_ && controls_.manualActive()) {
        releaseInputs();
        return;
    }
    pressKey(kKeyW, false);
    pressKey(kKeyS, false);
    pressKey(kKeyA, false);
    pressKey(kKeyD, false);
    pressKey(kKeySprint, false);
    ++smeltTicks_;
    switch (smeltStage_) {
    case SmeltStage::CraftFurnace: {
        if (smeltFurnaceGhost_) {
            if (chainActive_)
                return;
            if (!furnaceChainTried_) {
                furnaceChainTried_ = true;
                if (tryStartFurnaceChain())
                    return;
                smeltFail("かまどを作れなかった");
                return;
            }
            if (countAllItems("furnace") >= 1) {
                smeltFurnaceGhost_ = false;
                smeltAdvance();
            } else if (smeltTicks_ > 20 * 30)
                smeltFail("かまどを作れなかった");
            return;
        }
        if (countAllItems("furnace") >= 1) {
            smeltAdvance();
            return;
        }
        if (chainActive_)
            return; // the furnace chain is running (phase Craft)
        if (!furnaceChainTried_) {
            furnaceChainTried_ = true;
            if (tryStartFurnaceChain())
                return;
        }
        if (smeltTicks_ > 20 * 30)
            smeltFail("かまどを作れなかった");
        return;
    }
    case SmeltStage::PlaceFurnace:
        tickPlaceFurnace(feet);
        return;
    case SmeltStage::OpenFurnace:
        tickOpenFurnace(feet);
        return;
    case SmeltStage::LoadInput:
        (void)tickSmeltTransfer(25, 0, false);
        return;
    case SmeltStage::LoadFuel:
        (void)tickSmeltTransfer(24, 1, true);
        return;
    case SmeltStage::CloseUi:
        if (backgroundCraft_) {
            if (craft_.backgroundClose())
                smeltAdvance();
            return;
        }
        if (!anyScreenUp()) {
            smeltAdvance();
            return;
        }
        if (smeltTicks_ == 1 || smeltTicks_ == 30)
            pressKey(0x1B, true);
        if (smeltTicks_ == 3 || smeltTicks_ == 32)
            pressKey(0x1B, false);
        if (smeltTicks_ > 70)
            smeltAdvance();
        return;
    case SmeltStage::WaitSmelt:
        if (smeltTicks_ > smeltMoved_ * 200 + 150)
            smeltAdvance();
        return;
    case SmeltStage::BreakFurnace: {
        const int r = digCell(smeltFurnacePos_, feet);
        if (r == 1) {
            note("smelt: furnace broken, collecting");
            smeltAdvance();
        } else if (r == 2) {
            if (++smeltBreakRetries_ <= 3) {
                digPos_ = {INT_MIN, 0, 0};
                digBlockId_.clear();
                digTicks_ = 0;
                note("smelt: break refused, retrying");
            } else
                smeltFail("かまどを壊せなかった");
        }
        return;
    }
    case SmeltStage::CollectDrops: {
        const int gained = countAllItems(smeltYieldItem_) - smeltYieldBase_;
        if (gained >= smeltExpect_) {
            pressKey(kKeyW, false);
            reply("精錬完了！" + smeltYieldItem_ + " が " + std::to_string(gained) + "個できたよ");
            note("smelt: done, " + std::to_string(gained) + "x " + smeltYieldItem_);
            smeltDone_ = true;
            smeltStage_ = SmeltStage::Done;
            phase_ = Phase::Idle;
            return;
        }
        const bedrock::Vec3 eye{feet.x, feet.y + 1.62f, feet.z};
        const bedrock::Vec3 center{smeltFurnacePos_.x + .5f, feet.y + .5f,
                                   smeltFurnacePos_.z + .5f};
        faceAim(eye, center, 12.f, 0.3f);
        pressKey(kKeyW, (smeltTicks_ % 40) < 25);
        if (smeltTicks_ > 240) {
            pressKey(kKeyW, false);
            note("smelt diag: collect shortfall, gained=" + std::to_string(gained) + " expect=" +
                 std::to_string(smeltExpect_) + " base=" + std::to_string(smeltYieldBase_));
            smeltFail("精錬物を取りこぼした");
        }
        return;
    }
    case SmeltStage::Done:
    case SmeltStage::Failed:
        phase_ = Phase::Idle;
        return;
    }
}

void AutoMineMod::tickPlaceFurnace(bedrock::Vec3 feet) {
    if (backgroundCraft_)
        setUse(false);
    {
        bedrock::Vec3 eye0;
        if (eyePos(eye0)) {
            const int fx = static_cast<int>(std::floor(feet.x));
            const int fy = static_cast<int>(std::floor(feet.y));
            const int fz = static_cast<int>(std::floor(feet.z));
            for (int dy = 1; dy >= -1; --dy) {
                const BlockPos3 nbs[8] = {{fx + 1, fy + dy, fz},     {fx - 1, fy + dy, fz},
                                          {fx, fy + dy, fz + 1},     {fx, fy + dy, fz - 1},
                                          {fx + 1, fy + dy, fz + 1}, {fx + 1, fy + dy, fz - 1},
                                          {fx - 1, fy + dy, fz + 1}, {fx - 1, fy + dy, fz - 1}};
                for (const auto &nb : nbs) {
                    if (!contains(localPart(blockAt(nb)), "furnace"))
                        continue;
                    BlockPos3 occ;
                    if (!lineOfSight(eye0, nb, occ))
                        continue;
                    smeltFurnacePos_ = nb;
                    smeltPlaced_ = true;
                    note("smelt: adopting nearby furnace");
                    smeltAdvance();
                    return;
                }
            }
        }
    }
    if (smeltTicks_ > 20 * 25) {
        smeltFail("かまどを置けなかった");
        return;
    }
    if (!backgroundCraft_ && anyScreenUp() && moveStage_ == MoveStage::None) {
        if (tickCount_ % 30 == 1)
            pressKey(0x1B, true);
        if (tickCount_ % 30 == 3)
            pressKey(0x1B, false);
        return;
    }
    if (smeltPocketing_) {
        const int r = digPocketTick(feet, smeltPocket_, smeltPocketBad_, smeltPocketStage_);
        if (r == 2) {
            smeltPocketing_ = false;
            smeltFail("かまどを置く場所が無い");
            return;
        }
        if (r == 1) {
            smeltPocketing_ = false;
            smeltPocketDug_ = true;
            smeltPocketCell_ = smeltPocket_;
        } else
            return;
    }
    bedrock::Vec3 eye;
    if (!eyePos(eye))
        return;
    if (smeltPlaceCell_.x == INT_MIN) {
        const int fx = static_cast<int>(std::floor(feet.x));
        const int fy = static_cast<int>(std::floor(feet.y));
        const int fz = static_cast<int>(std::floor(feet.z));
        std::array<BlockPos3, 4> dirs{{{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}}};
        smeltPlaceCells_.clear();
        for (const auto &d : dirs) {
            const BlockPos3 cell{fx + d.x, fy, fz + d.z};
            if (workstationFailedCells_.count(cell))
                continue;
            if (cell.x < feet.x + .31 && cell.x + 1 > feet.x - .31 && cell.z < feet.z + .31 &&
                cell.z + 1 > feet.z - .31 &&
                !(smeltPocketDug_ && cell.x == smeltPocketCell_.x && cell.y == smeltPocketCell_.y &&
                  cell.z == smeltPocketCell_.z))
                continue;
            const auto at = blockAt(cell);
            if (contains(localPart(at), "furnace")) {
                BlockPos3 occ;
                if (!lineOfSight(eye, cell, occ))
                    continue;
                smeltFurnacePos_ = cell;
                smeltPlaced_ = true;
                note("smelt: adopting existing furnace");
                smeltAdvance();
                return;
            }
            if (!workstationCellReady(cell, eye))
                continue;
            if (isGravityBlock(localPart(blockAt({cell.x, cell.y + 1, cell.z}))))
                continue;
            BlockPos3 occluder;
            const bool isPocketCell = smeltPocketDug_ && cell.x == smeltPocketCell_.x &&
                                      cell.y == smeltPocketCell_.y && cell.z == smeltPocketCell_.z;
            if (!isPocketCell && !lineOfSight(eye, {cell.x, cell.y - 1, cell.z}, occluder))
                continue;
            smeltPlaceCells_.push_back(cell);
        }
        if (smeltPlaceCells_.empty()) {
            if (smeltPocketDug_) {
                smeltFail("かまどを置く場所が無い");
                return;
            }
            smeltPocketing_ = true;
            smeltPocket_ = {INT_MIN, 0, 0};
            smeltPocketStage_ = 0;
            note("smelt: digging a pocket for the furnace");
            return;
        }
        smeltPlaceIdx_ = 0;
        smeltCellTicks_ = 0;
        smeltPlaceCell_ = smeltPlaceCells_[0];
    }
    // (furnace placement shares the table placement pattern)
    if (contains(localPart(blockAt(smeltPlaceCell_)), "furnace")) {
        smeltFurnacePos_ = smeltPlaceCell_;
        smeltPlaced_ = true;
        note("smelt: furnace placed");
        smeltAdvance();
        return;
    }
    {
        // Same refill guard as the table placement.
        if (!workstationCellReady(smeltPlaceCell_, eye)) {
            smeltCellTicks_ = 31;
        }
    }
    int slot = findHotbarSlot("furnace");
    if (moveStage_ != MoveStage::None) {
        if (!ensureHotbarTick("furnace"))
            return;
        slot = findHotbarSlot("furnace");
    } else if (slot < 0 && countAllItems("furnace") >= 1 && !moveFailed_) {
        if (!ensureHotbarTick("furnace"))
            return;
        slot = findHotbarSlot("furnace");
    }
    if (slot < 0) {
        // Wait for the inventory view to settle, then retry the hotbar move once.
        if (++smeltNoSlotTicks_ <= 100)
            return;
        if (smeltNoSlotRetries_ < 1 && countAllItems("furnace") >= 1) {
            ++smeltNoSlotRetries_;
            smeltNoSlotTicks_ = 0;
            moveFailed_ = false;
            moveStage_ = MoveStage::None;
            note("smelt: furnace counted but no hotbar slot; resetting the move and retrying");
            return;
        }
        if (!smeltFurnaceGhost_) {
            smeltFurnaceGhost_ = true;
            furnaceChainTried_ = false;
            smeltStage_ = SmeltStage::CraftFurnace;
            smeltTicks_ = 0;
            smeltNoSlotTicks_ = 0;
            smeltNoSlotRetries_ = 0;
            moveFailed_ = false;
            note("smelt: counted furnace not in hotbar (ghost?); crafting one");
            return;
        }
        moveFailed_ = false;
        smeltFail("かまどがホットバーに無い");
        return;
    }
    smeltNoSlotTicks_ = 0;
    if (!smeltPlaceArmed_) {
        // Same budget decoupling as tickPlaceTable: the clock
        // starts when the furnace is actually in hand.
        smeltPlaceArmed_ = true;
        smeltTicks_ = 0;
    }
    // Same claimed-slot rotation as tickPlaceTable.
    slot = placeSlotTick("furnace", smeltPlaceSlotIdx_, smeltPlaceSlotTicks_, slot);
    selectSlot(slot);
    const bedrock::Vec3 aim{smeltPlaceCell_.x + .5f, static_cast<float>(smeltPlaceCell_.y),
                            smeltPlaceCell_.z + .5f};
    faceToward(eye, aim, 3.f);
    pressKey(kKeyS, smeltPlaceCell_.x < feet.x + .31f && smeltPlaceCell_.x + 1 > feet.x - .31f &&
                        smeltPlaceCell_.z < feet.z + .31f && smeltPlaceCell_.z + 1 > feet.z - .31f);
    if (smeltCellTicks_ == 0) {
        if (controls_.manualActive() || anyScreenUp() || tickCount_ - lastNativeUse_ < 4)
            return;
        if (!placeWorkstation(smeltPlaceCell_, eye))
            smeltCellTicks_ = 31;
    }
    else
        setUse(false);
    if (++smeltCellTicks_ > 30) {
        workstationFailedCells_.insert(smeltPlaceCell_);
        setUse(false);
        smeltCellTicks_ = 0;
        if (++smeltPlaceIdx_ >= smeltPlaceCells_.size()) {
            smeltFail("支持面のある設置候補を試したがかまどを置けなかった");
            return;
        }
        smeltPlaceCell_ = smeltPlaceCells_[smeltPlaceIdx_];
        note("smelt: trying another furnace cell");
    }
    if ((smeltTicks_ % 100) == 50) {
        note("place diag: furnace cell=" + std::to_string(smeltPlaceCell_.x) + "," +
             std::to_string(smeltPlaceCell_.y) + "," + std::to_string(smeltPlaceCell_.z) +
             " at=" + localPart(blockAt(smeltPlaceCell_)) + " ground=" +
             localPart(blockAt({smeltPlaceCell_.x, smeltPlaceCell_.y - 1, smeltPlaceCell_.z})) +
             " above=" +
             localPart(blockAt({smeltPlaceCell_.x, smeltPlaceCell_.y + 1, smeltPlaceCell_.z})) +
             " slot=" + std::to_string(slot) +
             " top=" + (screenReady_ ? topScreenName() : std::string("?")));
    }
}

void AutoMineMod::tickOpenFurnace(bedrock::Vec3 feet) {
    if (backgroundCraft_ &&
        craft_.backgroundOpen(bedrock::NativeItemStackRequests::BackgroundKind::Furnace)) {
        setUse(false);
        note("smelt: background furnace open (chat untouched)");
        smeltAdvance();
        return;
    }
    if (!smeltPlaced_) {
        smeltFail("かまどが無い");
        return;
    }
    if (smeltTicks_ > 20 * 6) {
        smeltFail("かまどを開けなかった");
        return;
    }
    bedrock::Vec3 eye;
    if (!eyePos(eye))
        return;
    const auto furnaceHit = automine::miningPoint(
        {eye.x, eye.y, eye.z}, {smeltFurnacePos_.x, smeltFurnacePos_.y, smeltFurnacePos_.z});
    const bedrock::Vec3 center{static_cast<float>(furnaceHit.x), static_cast<float>(furnaceHit.y),
                               static_cast<float>(furnaceHit.z)};
    BlockPos3 occ;
    if (!lineOfSight(eye, smeltFurnacePos_, occ)) {
        if (contains(localPart(blockAt(occ)), "furnace")) {
            note("smelt: adopting the furnace in the aim path");
            smeltFurnacePos_ = occ;
            smeltAimedTick_ = -1;
            return;
        }
        note("smelt: furnace shadowed by " + localPart(blockAt(occ)) + "; re-placing");
        smeltPlaced_ = false;
        smeltFail("かまどが他のブロックに隠れている");
        return;
    }
    if (backgroundCraft_) {
        setUse(false);
        if (controls_.manualActive())
            return;
        if (!automine::inMiningReach({eye.x, eye.y, eye.z}, {smeltFurnacePos_.x, smeltFurnacePos_.y,
                                                             smeltFurnacePos_.z})) {
            smeltFail("裏コンテナがリーチ外");
            return;
        }
        if (!craft_.backgroundActive()) {
            faceToward(eye, center, 3.f);
            BlockPos3 cell;
            unsigned face{};
            bedrock::Vec3 hit{};
            if (independentHit(eye, cell, face, hit) && cell == smeltFurnacePos_)
                (void)craft_.backgroundUse(
                    bedrock::NativeItemStackRequests::BackgroundKind::Furnace,
                    {cell.x, cell.y, cell.z}, face, eye, hit);
        }
        if (craft_.backgroundFailed())
            smeltFail("裏コンテナ応答なし");
        return;
    }
    if (smeltAimedTick_ < 0) {
        if (faceToward(eye, center, 3.f))
            smeltAimedTick_ = smeltTicks_;
        return;
    }

    const int since = smeltTicks_ - smeltAimedTick_;
    setUse(since < 2);
    // Advance only when the furnace screen is verifiably on top,
    // by NAME (see screenUp) — same fix as tickOpenTable.
    if (!backgroundCraft_ && screenUp("furnace")) {
        setUse(false);
        note("smelt: furnace open ('" + topScreenName() + "')");
        smeltAdvance();
        return;
    }
    if (!backgroundCraft_ && since > 15)
        smeltAimedTick_ = -1;
}

} // namespace automine::client
