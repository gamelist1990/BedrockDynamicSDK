#include "auto_mine.hpp"
#include "diagnostics.hpp"

namespace automine::client {

void AutoMineMod::planVein() {
    veinPlan_.clear();
    veinId_ = targetId_;
    if (!contains(targetId_, "_ore"))
        return;
    std::set<BlockPos3> seen;
    std::deque<BlockPos3> open{target_};
    constexpr int dirs[6][3]{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    while (!open.empty() && veinPlan_.size() < 32) {
        const auto c = open.front();
        open.pop_front();
        if (!seen.insert(c).second || blacklist_.count(c) || localPart(blockAt(c)) != veinId_)
            continue;
        veinPlan_.push_back(c);
        for (const auto &d : dirs)
            open.push_back({c.x + d[0], c.y + d[1], c.z + d[2]});
    }
}

void AutoMineMod::finishCollection(bedrock::Vec3 feet) {
    resetCollectionSweep();
    collectDropId_.clear();
    collectEntityValid_ = false;
    collectTarget_ = {INT_MIN, 0, 0};
    collectDig_.reset();
    pickupTarget_ = {INT_MIN, 0, 0};
    pressKey(kKeyW, false);
    pressKey(kKeySpace, false);
    if (goalReached()) {
        finishGoal();
        return;
    }
    const bool resume = !savedId_.empty();
    restoreMineThroughGoal();
    if (resume && localPart(blockAt(target_)) == targetId_) {
        hasTarget_ = true;
        navTicks_ = stuckTicks_ = replans_ = 0;
        phase_ = Phase::Navigate;
        return;
    }
    // Stay on the planned vein instead of starting a volume scan after
    // every diamond. Invalid cells are removed when the world changes.
    veinPlan_.erase(std::remove_if(veinPlan_.begin(), veinPlan_.end(),
                                   [this](const auto &c) {
                                       return blacklist_.count(c) ||
                                              localPart(blockAt(c)) != veinId_;
                                   }),
                    veinPlan_.end());
    auto best = veinPlan_.end();
    double score = 1e18;
    for (auto it = veinPlan_.begin(); it != veinPlan_.end(); ++it) {
        const double d =
            std::hypot(it->x + .5 - feet.x, it->y + .5 - feet.y - 1.62, it->z + .5 - feet.z);
        if (d < score) {
            score = d;
            best = it;
        }
    }
    if (best != veinPlan_.end()) {
        target_ = *best;
        targetId_ = veinId_;
        hasTarget_ = true;
        phase_ = Phase::Navigate;
        navTicks_ = 0;
        stuckTicks_ = 0;
        replans_ = 0;
        path_.clear();
        pathDig_.clear();
        pathBridge_.clear();
        pathParkour_.clear();
        pathPillar_.clear();
        pathFor_ = {INT_MIN, 0, 0};
        pathIdx_ = 0;
        return;
    }
    hasTarget_ = false;
    phase_ = Phase::Scan;
    scanCursor_ = 0;
    bestDist2_ = 1e18;
}

void AutoMineMod::tickScan(bedrock::Vec3 feet) {
    crashrec::Section section("scan");
    releaseInputs();
    if (scanWait_ > 0) {
        --scanWait_;
        return;
    }
    if (scanCursor_ == 0)
        scanExcludedMatch_ = false;
    const int cx = static_cast<int>(std::floor(feet.x));
    const int cy = static_cast<int>(std::floor(feet.y));
    const int cz = static_cast<int>(std::floor(feet.z));
    const int ry = 10;
    const int spanX = radius_ * 2 + 1, spanY = ry * 2 + 1, spanZ = radius_ * 2 + 1;
    const int total = spanX * spanY * spanZ;
    void *src = region();
    if (!src)
        return;
    int budget = 9000;
    while (scanCursor_ < total && budget-- > 0) {
        const int i = scanCursor_++;
        const int dx = (i % spanX) - radius_;
        const int dy = ((i / spanX) % spanY) - ry;
        const int dz = (i / (spanX * spanY)) - radius_;
        BlockPos3 pos{cx + dx, cy + dy, cz + dz};
        if (chainTablePlaced_ && pos.x == chainTablePos_.x && pos.y == chainTablePos_.y - 1 &&
            pos.z == chainTablePos_.z)
            continue;
        bedrock::BlockPos bp;
        bp.x = pos.x;
        bp.y = pos.y;
        bp.z = pos.z;
        auto cell = world_.getBlock(src, bp);
        if (cell.identifier.empty() || isAir(cell.identifier))
            continue;
        if (scanIds_.empty()) {
            if (localPart(cell.identifier) != targetId_)
                continue;
        } else if (std::find(scanIds_.begin(), scanIds_.end(), localPart(cell.identifier)) ==
                   scanIds_.end()) {
            continue;
        }
        if (blacklist_.count(pos)) {
            scanExcludedMatch_ = true;
            continue;
        }
        const double ddx = pos.x + .5 - feet.x, ddy = pos.y + .5 - feet.y,
                     ddz = pos.z + .5 - feet.z;
        const double d2 = ddx * ddx + ddy * ddy + ddz * ddz;
        // Rank usable approaches rather than the first nearby ore:
        // ascent and enclosed rock cost more than exposed, level work.
        if (std::sqrt(d2) >= bestDist2_)
            continue;
        if (skipUnderwater_) {
            bool underWater = false;
            for (int wy = pos.y + 1; wy <= pos.y + 16; ++wy) {
                bedrock::BlockPos above;
                above.x = pos.x;
                above.y = wy;
                above.z = pos.z;
                auto aboveCell = world_.getBlock(src, above);
                if (contains(aboveCell.identifier, "water")) {
                    underWater = true;
                    break;
                }
            }
            if (underWater)
                continue;
        }
        bool inHazard = false;
        for (const auto &z : hazardZones_) {
            if (std::abs(pos.x - z.x) <= z.r && std::abs(pos.y - z.y) <= z.r &&
                std::abs(pos.z - z.z) <= z.r) {
                inHazard = true;
                break;
            }
        }
        if (inHazard)
            continue;
        int exposed = 0;
        constexpr int around[6][3]{{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                   {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
        for (const auto &d : around)
            if (isPassableCell(blockAt({pos.x + d[0], pos.y + d[1], pos.z + d[2]})))
                ++exposed;
        const double score = std::sqrt(d2) + std::max(0.0, ddy - 3.0) * 2.0 +
                             (exposed ? 0.0 : blockDigCost(cell.identifier));
        if (score < bestDist2_) {
            bestDist2_ = score;
            target_ = pos;
            hasTarget_ = true;
            scanFoundId_ = localPart(cell.identifier);
        }
    }
    if (scanCursor_ < total)
        return;
    if (hasTarget_) {
        emptyScans_ = 0;
        if (!scanIds_.empty()) {
            if (!scanOverrideHeld_) {
                scanSavedId_ = targetId_;
                scanOverrideHeld_ = true;
            }
            targetId_ = scanFoundId_;
        }
        note("target: " + targetId_ + " at " + std::to_string(target_.x) + "," +
             std::to_string(target_.y) + "," + std::to_string(target_.z));
        planVein();
        phase_ = Phase::Navigate;
        stuckTicks_ = 0;
        lastX_ = feet.x;
        lastZ_ = feet.z;
        navTicks_ = 0;
        pathFor_ = {INT_MIN, 0, 0};
        replans_ = 0;
        path_.clear();
        pathDig_.clear();
        pathBridge_.clear();
        pathParkour_.clear();
        pathPillar_.clear();
        pathIdx_ = 0;
    } else {
        // Nothing in range: wait ~2s, then rescan (the player may move,
        // or a fresh area may load).
        note(scanExcludedMatch_ ? "scan: matching target remains, no reachable mining stance"
                                : "scan: no " + targetId_ + " in range");
        scanCursor_ = 0;
        bestDist2_ = 1e18;
        if (++emptyScans_ == 3) {
            if (scanExcludedMatch_)
                reply("対象ブロックは見つかったけど、採掘できる足場に到達できないよ。場所を変えてみて");
            else
                reply("近くに " + targetId_ + " が見つからないよ。場所を変えてみて");
        }
        scanWait_ = 40;
    }
}

} // namespace automine::client
