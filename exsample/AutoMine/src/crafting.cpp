#include "auto_mine.hpp"

namespace automine::client {

bool AutoMineMod::routeCrossesWater(const bedrock::Vec3 &feet, const bedrock::Vec3 &center) {
    const double dx = center.x - feet.x, dz = center.z - feet.z;
    const double len = std::hypot(dx, dz);
    if (len < 6.0)
        return false;
    const int by = static_cast<int>(std::floor(feet.y));
    for (double d = 2.0; d <= std::min(len, 9.0); d += 1.5) {
        const int bx = static_cast<int>(std::floor(feet.x + dx / len * d));
        const int bz = static_cast<int>(std::floor(feet.z + dz / len * d));
        if (contains(localPart(blockAt({bx, by, bz})), "water") ||
            contains(localPart(blockAt({bx, by - 1, bz})), "water"))
            return true;
    }
    return false;
}

bool AutoMineMod::placedTableExists() {
    if (chainTablePlaced_ && contains(localPart(blockAt(chainTablePos_)), "crafting_table") &&
        !tableBanned(chainTablePos_)) {
        bedrock::Vec3 eye;
        BlockPos3 obstruction;
        if (eyePos(eye) &&
            automine::inMiningReach({eye.x, eye.y, eye.z},
                                    {chainTablePos_.x, chainTablePos_.y, chainTablePos_.z}) &&
            lineOfSight(eye, chainTablePos_, obstruction))
            return true;
    }
    bedrock::Vec3 feet;
    if (!feetPos(feet))
        return false;
    const int fx = static_cast<int>(std::floor(feet.x));
    const int fy = static_cast<int>(std::floor(feet.y));
    const int fz = static_cast<int>(std::floor(feet.z));
    BlockPos3 best{INT_MIN, 0, 0};
    bedrock::Vec3 tableEye;
    if (!eyePos(tableEye))
        return false;
    double bestD2 = 36.0;
    for (int dx = -6; dx <= 6; ++dx)
        for (int dy = -2; dy <= 2; ++dy)
            for (int dz = -6; dz <= 6; ++dz) {
                const double d2 = static_cast<double>(dx * dx + dy * dy + dz * dz);
                if (d2 > bestD2)
                    continue;
                const BlockPos3 c{fx + dx, fy + dy, fz + dz};
                if (tableBanned(c))
                    continue;
                if (!contains(localPart(blockAt(c)), "crafting_table"))
                    continue;
                BlockPos3 obstruction;
                if (!automine::inMiningReach({tableEye.x, tableEye.y, tableEye.z},
                                             {c.x, c.y, c.z}) ||
                    !lineOfSight(tableEye, c, obstruction))
                    continue;
                best = c;
                bestD2 = d2;
            }
    if (best.x == INT_MIN)
        return false;
    chainTablePos_ = best;
    chainTablePlaced_ = true;
    note("chain: reusing placed table at " + std::to_string(best.x) + "," + std::to_string(best.y) +
         "," + std::to_string(best.z));
    return true;
}

int AutoMineMod::findTableHotbarSlot() {
    const auto slots = hotbarItems();
    for (int i = 0; i < 9; ++i) {
        const auto &it = slots[static_cast<std::size_t>(i)];
        if (it.count && localPart(it.id) == "crafting_table")
            return i;
    }
    return -1;
}

std::string AutoMineMod::chainWood(const std::unordered_map<std::string, int> &items,
                                   int &logCount) {
    std::string best;
    logCount = 0;
    for (const auto &[id, count] : items) {
        std::string wood;
        for (const char *suffix : {"_log", "_wood", "_stem", "_hyphae"})
            if (id.size() > std::string_view(suffix).size() &&
                id.compare(id.size() - std::string_view(suffix).size(),
                           std::string_view(suffix).size(), suffix) == 0)
                wood = id.substr(0, id.size() - std::string_view(suffix).size());
        if (wood.starts_with("stripped_"))
            wood.erase(0, 9);
        if (!wood.empty() && count > logCount) {
            logCount = count;
            best = wood;
        }
    }
    return best;
}

bool AutoMineMod::tryStartUtilityChain() {
    if (!craftReady_ || !craft_.ready() || !craft_.recipesReady() || chainActive_ ||
        tickCount_ < utilityNext_)
        return false;
    utilityNext_ = tickCount_ + 40;
    const auto have = chainCounts();
    const bool table = placedTableExists();
    const int tables = countIn(have, "crafting_table"), planks = countIn(have, "planks"),
              sticks = countIn(have, "stick");
    int logs{};
    const auto wood = chainWood(have, logs);
    const auto hasTool = [&](int slot) {
        for (const auto &[id, count] : have)
            if (count > 0 && preferredToolSlot(id) == slot)
                return true;
        return false;
    };
    // Build the table from the first log; resume logging before trying
    // the axe, instead of waiting for the expedition's full 20 logs.
    const bool tableFirst = !table && tables < 1;
    int tool = -1;
    if (!tableFirst)
        for (int slot : {1, 2, 0, 3})
            if (!hasTool(slot)) {
                tool = slot;
                break;
            }
    if (!tableFirst && tool < 0)
        return false;
    const int units = tool == 3 ? 1 : tool == 0 ? 2 : 3;
    const int stickNeed = tool == 0 ? 1 : 2;
    const bool stone = tool >= 0 && countIn(have, "cobblestone") >= units;
    const int stickCrafts = tool < 0 ? 0 : std::max(0, (stickNeed - sticks + 3) / 4);
    const int plankNeed =
        (tableFirst ? 4 : 0) + stickCrafts * 2 + ((tool >= 0 && !stone) ? units : 0);
    const int logCrafts = std::max(0, (plankNeed - planks + 3) / 4);
    if (logCrafts > 0 && (wood.empty() || logs < logCrafts))
        return false;
    std::vector<ChainStep> steps;
    if (logCrafts)
        steps.push_back({ChainStep::Kind::Craft, "minecraft:" + wood + "_planks",
                         "minecraft:" + wood + "_planks", logCrafts, false});
    if (tableFirst)
        steps.push_back({ChainStep::Kind::Craft, "minecraft:WorkBench_recipeId",
                         "minecraft:crafting_table", 1, false});
    if (stickCrafts)
        steps.push_back(
            {ChainStep::Kind::Craft, "minecraft:stick", "minecraft:stick", stickCrafts, false});
    if (!steps.empty()) {
        steps.insert(steps.begin(), {ChainStep::Kind::OpenInventory});
        steps.push_back({ChainStep::Kind::CloseUi});
    }
    if (!table)
        steps.push_back({ChainStep::Kind::PlaceTable});
    if (tool >= 0) {
        const std::string suffix = tool == 0   ? "sword"
                                   : tool == 1 ? "axe"
                                   : tool == 2 ? "pickaxe"
                                               : "shovel";
        const std::string item = "minecraft:" + std::string(stone ? "stone_" : "wooden_") + suffix;
        bedrock::CraftRecipe recipe;
        if (!craft_.copyRecipe(item, recipe))
            return false;
        steps.push_back({ChainStep::Kind::OpenTable});
        steps.push_back({ChainStep::Kind::Craft, item, item, 1, true});
        steps.push_back({ChainStep::Kind::CloseUi});
        utilityOutput_ = item;
    } else
        utilityOutput_ = "minecraft:crafting_table";
    chainSteps_ = std::move(steps);
    chainStep_ = 0;
    chainActive_ = true;
    chainJobStarted_ = false;
    chainForUtility_ = true;
    chainForPickaxe_ = false;
    chainForFurnace_ = false;
    chainReturn_ = Phase::Scan;
    chainStepTicks_ = 0;
    chainAimedTick_ = -1;
    chainRetriedOpen_ = false;
    chainRecoverTicks_ = -1;
    chainPendingFail_.clear();
    chainWaitingCobble_ = false;
    chainPlaceCell_ = {INT_MIN, 0, 0};
    chainPlaceCells_.clear();
    chainPlaceIdx_ = 0;
    placeCellTicks_ = 0;
    chainPocketing_ = false;
    chainPocketDug_ = false;
    chainPocketBad_ = 0;
    chainPocket_ = {INT_MIN, 0, 0};
    hasTarget_ = false;
    phase_ = Phase::Craft;
    releaseInputs();
    note("tool bootstrap: preparing " + utilityOutput_);
    return true;
}

bool AutoMineMod::tryStartPickaxeChain(bool force) {
    if (!craftReady_ || !craft_.recipesReady() || !craft_.ready() || chainActive_)
        return false;
    if (craft_.status() == bedrock::CraftJobStatus::Running)
        return false; // retry later
    craft_.clearStatus();
    const auto have = chainCounts();
    const int planks = countIn(have, "planks");
    const int sticks = countIn(have, "stick");
    const int tables = countIn(have, "crafting_table");
    const bool placed = placedTableExists();
    const int cobble = countIn(have, "cobblestone");
    const int goalTier = needTier_ >= 4 ? needTier_ : 2;
    int bestPickTier = 0;
    for (const auto &[id, count] : have) {
        if (!count || !contains(id, "_pickaxe"))
            continue;
        int t{};
        if (classForItem(id, t) == ToolClass::Pickaxe)
            bestPickTier = std::max(bestPickTier, t);
    }
    if (bestPickTier >= goalTier && !force)
        return false; // a usable pickaxe exists somewhere; not our problem
    auto exactIn = [&](const char *id) {
        const auto it = have.find(id);
        return it == have.end() ? 0 : it->second;
    };
    if (goalTier == 4 && exactIn("iron_ingot") < 3)
        return false;
    if (goalTier == 5 && exactIn("diamond") < 3)
        return false;
    int logCount = 0;
    const std::string wood = chainWood(have, logCount);
    std::vector<ChainStep> steps;
    const int plankNeed = (goalTier >= 4 ? 0 : 3) + (sticks < 2 ? 2 : 0) +
                         (tables < 1 && !placed ? 4 : 0);
    const int deficit = plankNeed - planks;
    const int logCrafts = deficit > 0 ? (deficit + 3) / 4 : 0;
    if (logCrafts > 0) {
        if (wood.empty() || logCount < logCrafts)
            return false;
        steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:" + wood + "_planks",
                                  "minecraft:" + wood + "_planks", logCrafts, false});
    }
    if (sticks < 2)
        steps.push_back(
            ChainStep{ChainStep::Kind::Craft, "minecraft:stick", "minecraft:stick", 1, false});
    if (tables < 1 && !placed)
        steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:WorkBench_recipeId",
                                  "minecraft:crafting_table", 1, false});
    if (!steps.empty()) {
        steps.insert(steps.begin(), ChainStep{ChainStep::Kind::OpenInventory});
        steps.push_back(ChainStep{ChainStep::Kind::CloseUi});
    }
    if (!placed)
        steps.push_back(ChainStep{ChainStep::Kind::PlaceTable});
    if (goalTier >= 4) {
        const std::string pick =
            goalTier == 4 ? "minecraft:iron_pickaxe" : "minecraft:diamond_pickaxe";
        steps.push_back(ChainStep{ChainStep::Kind::OpenTable});
        steps.push_back(ChainStep{ChainStep::Kind::Craft, pick, pick, 1, true});
        steps.push_back(ChainStep{ChainStep::Kind::CloseUi});
    } else {
        steps.push_back(ChainStep{ChainStep::Kind::OpenTable});
        steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:wooden_pickaxe",
                                  "minecraft:wooden_pickaxe", 1, true});
        steps.push_back(ChainStep{ChainStep::Kind::CloseUi});
        if (cobble < 3)
            steps.push_back(ChainStep{ChainStep::Kind::WaitCobble});
        steps.push_back(ChainStep{ChainStep::Kind::OpenTable});
        steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:stone_pickaxe",
                                  "minecraft:stone_pickaxe", 1, true});
        steps.push_back(ChainStep{ChainStep::Kind::CloseUi});
    }
    chainSteps_ = std::move(steps);
    chainStep_ = 0;
    chainActive_ = true;
    chainJobStarted_ = false;
    chainPlaceCell_ = {INT_MIN, 0, 0};
    chainPlaceCells_.clear();
    chainPlaceIdx_ = 0;
    placeCellTicks_ = 0;
    chainPlaceSlotIdx_ = 0;
    chainPlaceSlotTicks_ = 0;
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
    chainForPickaxe_ = true;
    chainReturn_ = Phase::Scan;
    phase_ = Phase::Craft;
    reply(goalTier == 4   ? "鉄のツルハシをクラフトするね"
          : goalTier == 5 ? "ダイヤのツルハシをクラフトするね"
                          : "材料があるから、ツルハシをクラフトするね");
    note("craft chain: pickaxe tier " + std::to_string(goalTier) + " (" +
         std::to_string(chainSteps_.size()) + " steps)");
    return true;
}

bool AutoMineMod::tryStartSwordChain() {
    if (!craftReady_ || !craft_.recipesReady() || !craft_.ready() || chainActive_)
        return false;
    if (craft_.status() == bedrock::CraftJobStatus::Running)
        return false;
    craft_.clearStatus();
    const auto have = chainCounts();
    if (countIn(have, "cobblestone") < 2)
        return false;
    const int tables = countIn(have, "crafting_table");
    const bool placed = placedTableExists();
    const int sticks = countIn(have, "stick");
    std::vector<ChainStep> steps;
    {
        const int plankNeed = (sticks < 1 ? 2 : 0) + ((tables < 1 && !placed) ? 4 : 0);
        int planks = countIn(have, "planks");
        if (planks < plankNeed) {
            int logCount = 0;
            const std::string wood = chainWood(have, logCount);
            const int logCrafts = (plankNeed - planks + 3) / 4;
            if (wood.empty() || logCount < logCrafts)
                return false;
            steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:" + wood + "_planks",
                                      "minecraft:" + wood + "_planks", logCrafts, false});
            planks += logCrafts * 4;
        }
        if (sticks < 1)
            steps.push_back(
                ChainStep{ChainStep::Kind::Craft, "minecraft:stick", "minecraft:stick", 1, false});
        if (tables < 1 && !placed)
            steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:WorkBench_recipeId",
                                      "minecraft:crafting_table", 1, false});
        if (!steps.empty()) {
            steps.insert(steps.begin(), ChainStep{ChainStep::Kind::OpenInventory});
            steps.push_back(ChainStep{ChainStep::Kind::CloseUi});
        }
    }
    if (!placed)
        steps.push_back(ChainStep{ChainStep::Kind::PlaceTable});
    steps.push_back(ChainStep{ChainStep::Kind::OpenTable});
    steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:stone_sword",
                              "minecraft:stone_sword", 1, true});
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
    chainReturn_ = Phase::Scan;
    phase_ = Phase::Craft;
    note("craft chain: stone sword (weapon prep)");
    return true;
}

void AutoMineMod::ensureSwordPrep() {
    if (!craftReady_)
        return;
    std::string mainSword;
    for (const auto &s : craft_.snapshot()) {
        if (!s.count || !contains(localPart(s.item), "_sword"))
            continue;
        if (s.slot < 9)
            return; // already in the hotbar
        mainSword = localPart(s.item);
    }
    if (!mainSword.empty()) {
        (void)ensureHotbarTick(mainSword);
        note("weapon prep: moving " + mainSword + " to the hotbar");
        return;
    }
    (void)tryStartSwordChain();
}

bool AutoMineMod::tryStartBoatChain() {
    if (!craftReady_ || !craft_.recipesReady() || !craft_.ready() || chainActive_)
        return false;
    if (craft_.status() == bedrock::CraftJobStatus::Running)
        return false;
    craft_.clearStatus();
    boatChainTriedFor_ = target_; // one attempt per target from here on
    const auto have = chainCounts();
    const int planks = countIn(have, "planks");
    const int tables = countIn(have, "crafting_table");
    const bool placed = placedTableExists();
    int logCount = 0;
    const std::string wood = chainWood(have, logCount);
    // Boat wood follows the planks that will actually be consumed: the
    // most plentiful planks, else the logs this chain turns into planks.
    std::string boatWood = wood;
    int bestPlanks = 0;
    for (const auto &[id, count] : have)
        if (contains(id, "_planks") && count > bestPlanks) {
            bestPlanks = count;
            boatWood = id.substr(0, id.size() - std::string_view("_planks").size());
        }
    if (boatWood.empty())
        return false;
    std::vector<ChainStep> steps;
    const int plankNeed = 5 + ((tables < 1 && !placed) ? 4 : 0);
    const int deficit = plankNeed - planks;
    const int logCrafts = deficit > 0 ? (deficit + 3) / 4 : 0;
    if (logCrafts > 0) {
        if (wood.empty() || logCount < logCrafts)
            return false;
        steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:" + wood + "_planks",
                                  "minecraft:" + wood + "_planks", logCrafts, false});
    }
    if (tables < 1 && !placed)
        steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:WorkBench_recipeId",
                                  "minecraft:crafting_table", 1, false});
    if (!steps.empty()) {
        steps.insert(steps.begin(), ChainStep{ChainStep::Kind::OpenInventory});
        steps.push_back(ChainStep{ChainStep::Kind::CloseUi});
    }
    if (!placed)
        steps.push_back(ChainStep{ChainStep::Kind::PlaceTable});
    steps.push_back(ChainStep{ChainStep::Kind::OpenTable});
    steps.push_back(ChainStep{ChainStep::Kind::Craft, "minecraft:boat",
                              "minecraft:" + boatWood + "_boat", 1, true});
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
    chainReturn_ = Phase::Navigate;
    phase_ = Phase::Craft;
    reply("ボートが無いから、木からクラフトするね");
    note("craft chain: boat (" + std::to_string(chainSteps_.size()) + " steps)");
    return true;
}

void AutoMineMod::chainAdvance() {
    ++chainStep_;
    chainStepTicks_ = 0;
    chainAimedTick_ = -1;
    chainJobStarted_ = false;
    chainPlaceArmed_ = false;
    chainNoSlotRetries_ = 0;
    chainNoSlotTicks_ = 0;
    // A place step may be holding Use / backing off when the
    // block lands: never leak a held input into the next step.
    setUse(false);
    pressKey(kKeyS, false);
    chainPlaceCell_ = {INT_MIN, 0, 0};
    chainPlaceCells_.clear();
    chainPlaceIdx_ = 0;
    placeCellTicks_ = 0;
    chainPlaceSlotIdx_ = 0;
    chainPlaceSlotTicks_ = 0;
    chainPocketing_ = false;
    chainPocketDug_ = false;
    chainPocketBad_ = 0;
    chainPocket_ = {INT_MIN, 0, 0};
    if (chainStep_ >= chainSteps_.size())
        chainFinish();
}

void AutoMineMod::chainFinish() {
    chainActive_ = false;
    chainRetries_ = 0;
    chainNextRetryTick_ = 0;
    setUse(false);
    pressKey(kKeySpace, false);
    if (chainForBuilding_) {
        chainForBuilding_ = false;
        phase_ = chainReturn_;
        hotbarCacheTick_ = -1;
        note("building supply: planks ready");
    } else if (chainForUtility_) {
        chainForUtility_ = false;
        note("tool bootstrap: done " + utilityOutput_);
        scanCursor_ = 0;
        bestDist2_ = 1e18;
        hasTarget_ = false;
        phase_ = chainReturn_;
        maintenanceNext_ = tickCount_ + 10;
    } else if (chainForPickaxe_) {
        wantPickaxeChain_ = false;
        reply("ツルハシ完成！採掘を再開するね");
        note("craft chain: pickaxe done");
        phase_ = Phase::Scan;
        scanCursor_ = 0;
        bestDist2_ = 1e18;
        hasTarget_ = false;
    } else if (chainForFurnace_) {
        chainForFurnace_ = false;
        note("craft chain: furnace done");
        phase_ = chainReturn_;
    } else {
        reply("ボート完成！");
        note("craft chain: boat done");
        phase_ = chainReturn_;
    }
}

void AutoMineMod::chainFail(const std::string &why) {
    if (chainForBuilding_) {
        chainForBuilding_ = false;
        buildingCraftNext_ = tickCount_ + 200;
    }
    if (backgroundCraft_)
        (void)craft_.backgroundClose();
    if (chainForUtility_) {
        chainForUtility_ = false;
        utilityNext_ = tickCount_ + 400;
    }
    note("craft chain failed: " + why);
    chainActive_ = false;
    chainJobStarted_ = false;
    chainWaitingCobble_ = false;
    setUse(false);
    pressKey(kKeySpace, false);
    if (chainForPickaxe_) {
        if (++chainRetries_ <= 5) {
            chainNextRetryTick_ = tickCount_ + 20 * 20;
            if (chainRetries_ == 1)
                reply("クラフト失敗（" + why + "）。少し待って再挑戦するね");
        } else {
            wantPickaxeChain_ = false;
            reply("クラフト失敗（" + why + "）。手でツルハシを用意してね");
        }
        phase_ = Phase::NeedTool;
        needTicks_ = 0;
    } else {
        chainForFurnace_ = false;
        phase_ = chainReturn_;
    }
}

void AutoMineMod::dispatchCraftStep(const ChainStep &step) {
    if (localPart(step.outputName) == "crafting_table" &&
        (countAllItems("crafting_table") > 0 || placedTableExists())) {
        note("craft chain: table already available; skipping duplicate craft");
        chainAdvance();
        return;
    }
    bedrock::CraftRecipe recipe;
    if (!craft_.copyRecipe(step.recipeId, recipe)) {
        chainFail("レシピ未取得: " + step.recipeId);
        return;
    }
    const int base = step.tableGrid ? 32 : 28;
    const int width = step.tableGrid ? 3 : 2;
    if (!craft_.startCraft(recipe, step.outputName, step.times, base, width)) {
        std::string why = "クラフト開始失敗";
        if (craft_.status() == bedrock::CraftJobStatus::Failed) {
            why = craft_.statusDetail();
            craft_.clearStatus();
        }
        chainFail(why);
        return;
    }
    chainJobStarted_ = true;
    chainStepTicks_ = 0;
}

void AutoMineMod::tickCraft(bedrock::Vec3 feet) {
    if ((backgroundCraft_ && anyScreenUp()) || contains(topScreenName(), "chat")) {
        releaseInputs();
        return;
    }
    if (backgroundCraft_ && controls_.manualActive() && !chainJobStarted_) {
        releaseInputs();
        return;
    }
    pressKey(kKeyW, false);
    pressKey(kKeyS, false);
    pressKey(kKeyA, false);
    pressKey(kKeyD, false);
    pressKey(kKeySprint, false);
    pressKey(kKeySpace, inWaterNow(feet));
    if (headInWater(feet)) {
        if (++chainSubmerged_ >= 60) {
            pressKey(kKeySpace, false);
            chainFail("水中");
            startEscape();
            return;
        }
    } else
        chainSubmerged_ = 0;
    if (!chainActive_) {
        phase_ = chainReturn_;
        return;
    }
    if (chainRecoverTicks_ >= 0) {
        ++chainRecoverTicks_;
        if (backgroundCraft_ ? craft_.backgroundClose()
                             : (!anyScreenUp() || chainRecoverTicks_ > 40)) {
            craft_.resetAfterFailure();
            chainRecoverTicks_ = -1;
            const std::string why = chainPendingFail_;
            chainPendingFail_.clear();
            // A table-grid craft failing usually means the table
            // UI never opened: rewind to the open step, retry once.
            if (chainStep_ < chainSteps_.size() && chainSteps_[chainStep_].tableGrid &&
                !chainRetriedOpen_) {
                for (std::size_t i = chainStep_; i-- > 0;)
                    if (chainSteps_[i].kind == ChainStep::Kind::OpenTable) {
                        chainRetriedOpen_ = true;
                        chainStep_ = i;
                        chainStepTicks_ = 0;
                        chainAimedTick_ = -1;
                        note("craft chain: reopening table after failure: " + why);
                        return;
                    }
            }
            chainFail(why);
            return;
        }
        if (!backgroundCraft_ && (chainRecoverTicks_ == 1 || chainRecoverTicks_ == 20))
            pressKey(0x1B, true);
        if (!backgroundCraft_ && (chainRecoverTicks_ == 3 || chainRecoverTicks_ == 22))
            pressKey(0x1B, false);
        return;
    }
    ++chainStepTicks_;
    if (chainJobStarted_) {
        const auto st = craft_.status();
        if (st == bedrock::CraftJobStatus::Running) {
            if (chainStepTicks_ > 20 * 240)
                chainFail("クラフト応答なし");
            return;
        }
        chainJobStarted_ = false;
        if (st == bedrock::CraftJobStatus::Failed) {
            // Route through the clean-state recovery above; the
            // rewind-or-fail decision runs when it completes.
            chainPendingFail_ = craft_.statusDetail();
            craft_.clearStatus();
            chainRecoverTicks_ = 0;
            return;
        }
        craft_.clearStatus(); // Done
        hotbarCacheTick_ = -1;
        moveFailed_ = false; // a new result may now be moved into the hotbar
        chainAdvance();
        return;
    }
    if (chainStep_ >= chainSteps_.size()) {
        chainFinish();
        return;
    }
    const ChainStep &step = chainSteps_[chainStep_];
    switch (step.kind) {
    case ChainStep::Kind::Craft:
        if (chainStepTicks_ >= 10)
            dispatchCraftStep(step);
        break;
    case ChainStep::Kind::PlaceTable:
        tickPlaceTable(feet);
        break;
    case ChainStep::Kind::OpenTable:
        tickOpenTable(feet);
        break;
    case ChainStep::Kind::CloseUi:
        tickCloseUi();
        break;
    case ChainStep::Kind::OpenInventory:
        tickOpenInventory();
        break;
    case ChainStep::Kind::WaitCobble:
        ++chainStep_;
        chainStepTicks_ = 0;
        chainWaitingCobble_ = true;
        chainCobbleTicks_ = 0;
        chainReturnTrips_ = 0;
        cobbleNavFails_ = 0;
        cobbleDescend_ = false;
        scanIds_ = {"stone"};
        phase_ = Phase::Scan;
        scanCursor_ = 0;
        bestDist2_ = 1e18;
        hasTarget_ = false;
        reply("木のツルハシ完成。丸石を3つ集めるね");
        note("craft chain: waiting for cobblestone");
        break;
    }
}

} // namespace automine::client
