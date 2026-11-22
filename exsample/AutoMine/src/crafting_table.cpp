#include "auto_mine.hpp"

namespace automine::client {

int AutoMineMod::digPocketTick(bedrock::sdk::Vec3Value feet, BlockPos3 &pocket, int &badMask, int &stage) {
    static constexpr int kDX[4] = {1, -1, 0, 0};
    static constexpr int kDZ[4] = {0, 0, 1, -1};
    const int fx = static_cast<int>(std::floor(feet.x));
    const int fy = static_cast<int>(std::floor(feet.y));
    const int fz = static_cast<int>(std::floor(feet.z));
    if (pocket.x == INT_MIN) {
        stage = 0;
        for (int i = 0; i < 4; ++i) {
            if (badMask & (1 << i))
                continue;
            const BlockPos3 cell{fx + kDX[i], fy, fz + kDZ[i]};
            if (workstationFailedCells_.count(cell)) {
                badMask |= 1 << i;
                continue;
            }
            const auto at = blockAt(cell);
            if (contains(localPart(at), "bedrock")) {
                badMask |= 1 << i;
                continue;
            }
            const auto ground = blockAt({cell.x, cell.y - 1, cell.z});
            if (!isSolidCell(ground) || !automine::fullCubeSupport(localPart(ground)) ||
                contains(ground, "crafting_table") || contains(ground, "furnace") ||
                contains(ground, "chest") || contains(ground, "barrel")) {
                badMask |= 1 << i;
                continue;
            }
            // A gravity block in the cell itself, or directly above
            // it, refills the pocket the moment it is dug.
            if (isGravityBlock(localPart(at)) ||
                isGravityBlock(localPart(blockAt({cell.x, cell.y + 1, cell.z})))) {
                badMask |= 1 << i;
                continue;
            }
            pocket = cell;
            break;
        }
        if (pocket.x == INT_MIN) {
            std::string dbg = "pocket refused:";
            for (int i = 0; i < 4; ++i) {
                const BlockPos3 cell{fx + kDX[i], fy, fz + kDZ[i]};
                dbg += " [" + localPart(blockAt(cell)) +
                       "|g:" + localPart(blockAt({cell.x, cell.y - 1, cell.z})) +
                       "|a:" + localPart(blockAt({cell.x, cell.y + 1, cell.z})) + "]";
            }
            dbg +=
                " feet=" + std::to_string(fx) + "," + std::to_string(fy) + "," + std::to_string(fz);
            note(dbg);
            return 2;
        }
    }
    const bool intersects = pocket.x < feet.x + .31f && pocket.x + 1 > feet.x - .31f &&
                            pocket.z < feet.z + .31f && pocket.z + 1 > feet.z - .31f;
    if (intersects && stage < 60) {
        bedrock::sdk::Vec3Value eye;
        bool aimed = false;
        if (eyePos(eye)) {
            const bedrock::sdk::Vec3Value c{pocket.x + .5f, pocket.y + .5f, pocket.z + .5f};
            aimed = faceToward(eye, c, 0.15f);
        }
        pressKey(kKeyS, aimed);
        ++stage;
        return 0;
    }
    pressKey(kKeyS, false);
    const auto atNow = blockAt(pocket);
    if (isAir(atNow) || contains(localPart(atNow), "water")) {
        setAttack(false);
        return 1; // keep the finished cell in `pocket` for the caller
    }
    const int r = digCell(pocket, feet, /*toolOptional=*/true);
    if (r == 1) {
        setAttack(false);
        return 1; // same: caller reads the cell from `pocket`
    }
    if (r == 2) {
        const int dx = pocket.x - fx, dz = pocket.z - fz;
        for (int i = 0; i < 4; ++i)
            if (kDX[i] == dx && kDZ[i] == dz)
                badMask |= 1 << i;
        pocket = {INT_MIN, 0, 0};
        stage = 0;
    }
    return 0;
}

bool AutoMineMod::workstationCellReady(BlockPos3 cell, bedrock::sdk::Vec3Value eye) {
    const auto at = blockAt(cell);
    if (at.empty() ||
        (!isAir(at) && !isPassableCell(at) && !contains(localPart(at), "water")))
        return false;
    const BlockPos3 support{cell.x, cell.y - 1, cell.z};
    const auto id = localPart(blockAt(support));
    // Never activate a workstation while trying to place another one on it.
    if (!isSolidCell(id) || !automine::fullCubeSupport(id) || contains(id, "crafting_table") ||
        contains(id, "furnace") || contains(id, "chest") || contains(id, "barrel"))
        return false;
    const auto read = [this, cell](automine::PlanCell c) {
        if (c.x == cell.x && c.y == cell.y && c.z == cell.z)
            return automine::Terrain::Air;
        return planTerrain(c);
    };
    const auto anchor = [&](automine::PlanCell c) {
        return c.x == support.x && c.y == support.y && c.z == support.z;
    };
    const auto options =
        automine::workstationFaces({cell.x, cell.y, cell.z}, {eye.x, eye.y, eye.z}, read, anchor);
    return !options.empty();
}

bool AutoMineMod::placeWorkstation(BlockPos3 cell, bedrock::sdk::Vec3Value eye) {
    setUse(false);
    if (controls_.manualActive() || anyScreenUp() || tickCount_ - lastNativeUse_ < 4 ||
        !workstationCellReady(cell, eye))
        return false;
    void *p = player();
    if (!p)
        return false;
    const BlockPos3 support{cell.x, cell.y - 1, cell.z};
    const auto read = [this](automine::PlanCell c) { return planTerrain(c); };
    const auto anchor = [&](automine::PlanCell c) {
        return c == automine::PlanCell{support.x, support.y, support.z};
    };
    const auto faces = automine::workstationFaces(
        {cell.x, cell.y, cell.z}, {eye.x, eye.y, eye.z}, read, anchor);
    if (faces.empty())
        return false;
    const auto &option = faces.front();
    bool accepted{};
    lastNativeUse_ = tickCount_;
    return world_.build(
               p, world_.gameMode(p), {support.x, support.y, support.z}, option.face, eye,
               {static_cast<float>(option.hit.x), static_cast<float>(option.hit.y),
                static_cast<float>(option.hit.z)},
               &accepted) &&
           accepted;
}

void AutoMineMod::tickPlaceTable(bedrock::sdk::Vec3Value feet) {
    if (backgroundCraft_)
        setUse(false);
    {
        bedrock::sdk::Vec3Value eye0;
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
                    if (!contains(localPart(blockAt(nb)), "crafting_table"))
                        continue;
                    if (tableBanned(nb))
                        continue;
                    BlockPos3 occ;
                    if (!lineOfSight(eye0, nb, occ))
                        continue;
                    chainTablePos_ = nb;
                    chainTablePlaced_ = true;
                    note("craft chain: adopting nearby table");
                    chainAdvance();
                    return;
                }
            }
        }
    }
    if (chainStepTicks_ > 20 * 25) {
        chainFail("作業台を置けなかった");
        return;
    }
    if (!backgroundCraft_ && anyScreenUp() && moveStage_ == MoveStage::None) {
        if (tickCount_ % 30 == 1)
            pressKey(0x1B, true);
        if (tickCount_ % 30 == 3)
            pressKey(0x1B, false);
        return;
    }
    if (chainPocketing_) {
        const int r = digPocketTick(feet, chainPocket_, chainPocketBad_, chainPocketStage_);
        if (r == 2) {
            chainPocketing_ = false;
            chainFail("作業台を置く場所が無い");
            return;
        }
        if (r == 1) {
            chainPocketing_ = false;
            chainPocketDug_ = true;
            chainPocketCell_ = chainPocket_;
        } else
            return;
    }
    bedrock::sdk::Vec3Value eye;
    if (!eyePos(eye))
        return;
    if (chainPlaceCell_.x == INT_MIN) {
        const int fx = static_cast<int>(std::floor(feet.x));
        const int fy = static_cast<int>(std::floor(feet.y));
        const int fz = static_cast<int>(std::floor(feet.z));
        std::array<BlockPos3, 4> dirs{{{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}}};
        if (hasTarget_) {
            bedrock::sdk::Vec3Value want{target_.x + .5f - feet.x, 0, target_.z + .5f - feet.z};
            std::stable_sort(dirs.begin(), dirs.end(), [&](const BlockPos3 &a, const BlockPos3 &b) {
                return (a.x * want.x + a.z * want.z) > (b.x * want.x + b.z * want.z);
            });
        }
        chainPlaceCells_.clear();
        for (const auto &d : dirs) {
            const BlockPos3 cell{fx + d.x, fy, fz + d.z};
            if (workstationFailedCells_.count(cell))
                continue;
            const bool isPocketCell = chainPocketDug_ && cell.x == chainPocketCell_.x &&
                                      cell.y == chainPocketCell_.y && cell.z == chainPocketCell_.z;
            if (cell.x < feet.x + .31 && cell.x + 1 > feet.x - .31 && cell.z < feet.z + .31 &&
                cell.z + 1 > feet.z - .31 && !isPocketCell)
                continue;
            const auto at = blockAt(cell);
            if (contains(localPart(at), "crafting_table")) {
                BlockPos3 occ;
                if (!lineOfSight(eye, cell, occ))
                    continue;
                chainTablePos_ = cell;
                chainTablePlaced_ = true;
                note("craft chain: adopting existing table");
                chainAdvance();
                return;
            }
            if (!workstationCellReady(cell, eye))
                continue;
            if (isGravityBlock(localPart(blockAt({cell.x, cell.y + 1, cell.z}))))
                continue;
            BlockPos3 occluder;
            if (!isPocketCell && !lineOfSight(eye, {cell.x, cell.y - 1, cell.z}, occluder))
                continue;
            chainPlaceCells_.push_back(cell);
        }
        if (chainPlaceCells_.empty()) {
            if (chainPocketDug_) {
                std::string dbg = "place cells empty after pocket:";
                for (const auto &d : dirs) {
                    const BlockPos3 cell{fx + d.x, fy, fz + d.z};
                    dbg += " [" + localPart(blockAt(cell)) +
                           "|g:" + localPart(blockAt({cell.x, cell.y - 1, cell.z})) + "]";
                }
                dbg += " pocket=" + std::to_string(chainPocketCell_.x) + "," +
                       std::to_string(chainPocketCell_.y) + "," +
                       std::to_string(chainPocketCell_.z) + " feet=" + std::to_string(fx) + "," +
                       std::to_string(fy) + "," + std::to_string(fz);
                note(dbg);
                chainFail("作業台を置く場所が無い");
                return;
            }
            // No air neighbour (1-wide shaft): dig a side pocket
            // and collect candidates again.
            chainPocketing_ = true;
            chainPocket_ = {INT_MIN, 0, 0};
            chainPocketStage_ = 0;
            note("craft chain: digging a pocket for the table");
            return;
        }
        chainPlaceIdx_ = 0;
        placeCellTicks_ = 0;
        chainPlaceCell_ = chainPlaceCells_[0];
    }
    {
        const int fx = static_cast<int>(std::floor(feet.x));
        const int fy = static_cast<int>(std::floor(feet.y));
        const int fz = static_cast<int>(std::floor(feet.z));
        for (int dy = 1; dy >= -1; --dy) {
            const BlockPos3 ring[8] = {{fx + 1, fy + dy, fz},     {fx - 1, fy + dy, fz},
                                       {fx, fy + dy, fz + 1},     {fx, fy + dy, fz - 1},
                                       {fx + 1, fy + dy, fz + 1}, {fx + 1, fy + dy, fz - 1},
                                       {fx - 1, fy + dy, fz + 1}, {fx - 1, fy + dy, fz - 1}};
            for (const auto &nb : ring) {
                if (!contains(localPart(blockAt(nb)), "crafting_table"))
                    continue;
                if (tableBanned(nb))
                    continue;
                BlockPos3 occ;
                if (!lineOfSight(eye, nb, occ))
                    continue;
                chainTablePos_ = nb;
                chainTablePlaced_ = true;
                note("craft chain: adopting table placed beside the target cell");
                chainAdvance();
                return;
            }
        }
    }
    if (contains(localPart(blockAt(chainPlaceCell_)), "crafting_table")) {
        chainTablePos_ = chainPlaceCell_;
        chainTablePlaced_ = true;
        note("craft chain: table placed");
        chainAdvance();
        return;
    }
    {
        if (!workstationCellReady(chainPlaceCell_, eye)) {
            placeCellTicks_ = 31;
        }
    }
    int slot = findTableHotbarSlot();
    if (moveStage_ != MoveStage::None) {
        // Same orphaned-move guard as tickPlaceFurnace: drive any
        // in-flight hotbar move to completion before pulsing.
        if (!ensureHotbarTick("crafting_table"))
            return;
        slot = findTableHotbarSlot();
    } else if (slot < 0 && countAllItems("crafting_table") >= 1 && !moveFailed_) {
        // The crafted table sits in the main inventory (the
        // hotbar was full at deposit time): move it in first.
        if (!ensureHotbarTick("crafting_table"))
            return;
        slot = findTableHotbarSlot();
    }
    if (slot < 0) {
        if (++chainNoSlotTicks_ <= 100)
            return;
        if (chainNoSlotRetries_ < 1 && countAllItems("crafting_table") >= 1) {
            ++chainNoSlotRetries_;
            chainNoSlotTicks_ = 0;
            moveFailed_ = false;
            moveStage_ = MoveStage::None;
            note("craft chain: table counted but no hotbar slot; resetting the move and retrying");
            return;
        }
        moveFailed_ = false;
        chainFail("作業台がホットバーに無い");
        return;
    }
    chainNoSlotTicks_ = 0;
    if (!chainPlaceArmed_) {
        chainPlaceArmed_ = true;
        chainStepTicks_ = 0;
    }
    slot = placeSlotTick("crafting_table", chainPlaceSlotIdx_, chainPlaceSlotTicks_, slot);
    selectSlot(slot);
    const bedrock::sdk::Vec3Value aim{chainPlaceCell_.x + .5f, static_cast<float>(chainPlaceCell_.y),
                            chainPlaceCell_.z + .5f};
    faceToward(eye, aim, 3.f);
    pressKey(kKeyS, chainPlaceCell_.x < feet.x + .31f && chainPlaceCell_.x + 1 > feet.x - .31f &&
                        chainPlaceCell_.z < feet.z + .31f && chainPlaceCell_.z + 1 > feet.z - .31f);
    if (placeCellTicks_ == 0) {
        if (controls_.manualActive() || anyScreenUp() || tickCount_ - lastNativeUse_ < 4)
            return;
        if (!placeWorkstation(chainPlaceCell_, eye))
            placeCellTicks_ = 31;
    }
    else
        setUse(false);
    if (++placeCellTicks_ > 30) {
        workstationFailedCells_.insert(chainPlaceCell_);
        setUse(false);
        placeCellTicks_ = 0;
        if (++chainPlaceIdx_ >= chainPlaceCells_.size()) {
            chainFail("支持面のある設置候補を試したが作業台を置けなかった");
            return;
        }
        chainPlaceCell_ = chainPlaceCells_[chainPlaceIdx_];
        note("craft chain: trying another table cell");
    }
    if ((chainStepTicks_ % 100) == 50) {
        note("place diag: table cell=" + std::to_string(chainPlaceCell_.x) + "," +
             std::to_string(chainPlaceCell_.y) + "," + std::to_string(chainPlaceCell_.z) +
             " at=" + localPart(blockAt(chainPlaceCell_)) + " ground=" +
             localPart(blockAt({chainPlaceCell_.x, chainPlaceCell_.y - 1, chainPlaceCell_.z})) +
             " above=" +
             localPart(blockAt({chainPlaceCell_.x, chainPlaceCell_.y + 1, chainPlaceCell_.z})) +
             " slot=" + std::to_string(slot) +
             " top=" + (screenReady_ ? topScreenName() : std::string("?")));
    }
}

bool AutoMineMod::tableBanned(const BlockPos3 &p) {
    return tickCount_ < chainTableBadUntil_ && p == chainTableBad_;
}

void AutoMineMod::tickOpenTable(bedrock::sdk::Vec3Value feet) {
    if (backgroundCraft_ &&
        craft_.backgroundOpen(bedrock::sdk::BackgroundKindValue::Table)) {
        setUse(false);
        note("craft chain: background table open (chat untouched)");
        chainAdvance();
        return;
    }
    if (!chainTablePlaced_) {
        chainFail("作業台が無い");
        return;
    }
    if (chainStepTicks_ > 20 * 6) {
        chainTableBad_ = chainTablePos_;
        chainTableBadUntil_ = tickCount_ + 20 * 600;
        chainTablePlaced_ = false;
        note("craft chain: table would not open; banning it and re-placing elsewhere"
             " (top screen was '" +
             (screenReady_ ? topScreenName() : std::string("?")) + "')");
        chainFail("作業台を開けなかった");
        return;
    }
    bedrock::sdk::Vec3Value eye;
    if (!eyePos(eye))
        return;
    const auto tableHit = automine::miningPoint(
        {eye.x, eye.y, eye.z}, {chainTablePos_.x, chainTablePos_.y, chainTablePos_.z});
    const bedrock::sdk::Vec3Value center{static_cast<float>(tableHit.x), static_cast<float>(tableHit.y),
                               static_cast<float>(tableHit.z)};
    BlockPos3 occ;
    if (!lineOfSight(eye, chainTablePos_, occ)) {
        if (contains(localPart(blockAt(occ)), "crafting_table") && !tableBanned(occ)) {
            note("craft chain: adopting the table in the aim path");
            chainTablePos_ = occ;
            chainAimedTick_ = -1;
            return;
        }
        note("craft chain: table shadowed by " + localPart(blockAt(occ)) + "; re-placing");
        chainTableBad_ = chainTablePos_;
        chainTableBadUntil_ = tickCount_ + 20 * 600;
        chainTablePlaced_ = false;
        chainFail("作業台が他のブロックに隠れている");
        return;
    }
    if (backgroundCraft_) {
        setUse(false);
        if (controls_.manualActive())
            return;
        if (!automine::inMiningReach({eye.x, eye.y, eye.z},
                                     {chainTablePos_.x, chainTablePos_.y, chainTablePos_.z})) {
            chainFail("裏コンテナがリーチ外");
            return;
        }
        if (!craft_.backgroundActive()) {
            faceToward(eye, center, 3.f);
            BlockPos3 cell;
            unsigned face{};
            bedrock::sdk::Vec3Value hit{};
            if (independentHit(eye, cell, face, hit) && cell == chainTablePos_)
                (void)craft_.backgroundUse(bedrock::sdk::BackgroundKindValue::Table,
                                           {cell.x, cell.y, cell.z}, face, eye, hit);
        }
        if (craft_.backgroundFailed())
            chainFail("裏コンテナ応答なし");
        return;
    }
    if (chainAimedTick_ < 0) {
        if (faceToward(eye, center, 3.f))
            chainAimedTick_ = chainStepTicks_;
        return;
    }

    const int since = chainStepTicks_ - chainAimedTick_;
    setUse(since < 2);
    if (!backgroundCraft_ && screenUp("crafting")) {
        setUse(false);
        note("craft chain: table open ('" + topScreenName() + "')");
        chainAdvance();
        return;
    }
    if (!backgroundCraft_ && since > 15)
        chainAimedTick_ = -1;
}

void AutoMineMod::tickOpenInventory() {
    if (backgroundCraft_) {
        if (craft_.backgroundOpen(bedrock::sdk::BackgroundKindValue::Inventory)) {
            note("craft chain: background inventory open (chat untouched)");
            chainAdvance();
            return;
        }
        if (!craft_.backgroundInventory() && chainStepTicks_ > 120)
            chainFail("裏インベントリを準備できなかった");
        else if (craft_.backgroundFailed() || chainStepTicks_ > 160)
            chainFail("裏インベントリ応答なし");
        return;
    }
    if (screenUp("inventory")) {
        chainAdvance();
        return;
    }
    if ((chainStepTicks_ % 45) == 1)
        pressKey(0x45, true); // VK_E
    if ((chainStepTicks_ % 45) == 3)
        pressKey(0x45, false);
    if (chainStepTicks_ > 300)
        chainFail("インベントリを開けなかった");
}

void AutoMineMod::tickCloseUi() {
    if (backgroundCraft_) {
        if (craft_.backgroundClose())
            chainAdvance();
        return;
    }
    if (!anyScreenUp()) {
        chainAdvance();
        return;
    }
    if (chainStepTicks_ == 1 || chainStepTicks_ == 30)
        pressKey(0x1B, true); // VK_ESCAPE
    if (chainStepTicks_ == 3 || chainStepTicks_ == 32)
        pressKey(0x1B, false);
    if (chainStepTicks_ > 70)
        chainAdvance();
}

} // namespace automine::client
