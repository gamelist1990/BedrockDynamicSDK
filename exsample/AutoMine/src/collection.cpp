#include "auto_mine.hpp"
#include "collection_geometry.hpp"

namespace automine::client {

void AutoMineMod::resetCollectionSweep() {
    collectSweep_.clear();
    collectRoute_.clear();
    collectSweepIdx_ = 0;
    collectRouteIdx_ = 0;
    collectDwell_ = collectCandidateTicks_ = collectLandingTicks_ = 0;
}

void AutoMineMod::startCollection() {
    // An adjacent drop can enter inventory before we observe the broken block.
    collectDropId_ = pickupItem_;
    collectBase_ = pickupBase_;
    collectStableTicks_ = collectRetryTicks_ = 0;
    collectRetryReported_ = false;
    collectDig_.reset();
    collectDigFailed_.clear();
    resetCollectionSweep();
    phase_ = Phase::Collect;
}

void AutoMineMod::tickCollect(bedrock::Vec3 feet) {
    if (!collectDig_)
        setAttack(false);
    pressKey(kKeySneak, false);
    pressKey(kKeySprint, false);
    const auto stop = [&] {
        pressKey(kKeyW, false);
        pressKey(kKeySpace, false);
    };
    const int gain = collectDropId_.empty() ? 0 : invCountOf(collectDropId_) - collectBase_;
    if (collectDropId_.empty() || pickupConfirmed(gain, collectStableTicks_)) {
        setAttack(false);
        if (!collectDropId_.empty())
            note("collect: verified " + collectDropId_ + " +" + std::to_string(gain));
        finishCollection(feet);
        return;
    }
    if (collectStableTicks_ > 0 || collectRetryTicks_ > 0) {
        stop();
        setAttack(false);
        if (collectRetryTicks_ > 0)
            --collectRetryTicks_;
        return;
    }
    const auto skip = [&] {
        stop();
        ++collectSweepIdx_;
        collectCandidateTicks_ = collectDwell_ = collectLandingTicks_ = 0;
        collectRoute_.clear();
        collectRouteIdx_ = 0;
    };
    std::map<BlockPos3, PickupTile> terrainCache;
    const auto read = [&](PlanCell p) {
        const BlockPos3 pos{p.x, p.y, p.z};
        if (const auto it = terrainCache.find(pos); it != terrainCache.end())
            return it->second;
        const auto id = blockAt(pos);
        const auto local = localPart(id);
        auto tile = PickupTile::Obstacle;
        if (id.empty())
            tile = PickupTile::Unknown;
        else if (isPassableCell(id))
            tile = PickupTile::Air;
        else if (local.ends_with("_slab"))
            tile = PickupTile::Slab;
        else if (isSolidCell(id) && fullCubeSupport(local) && local != "magma" &&
                 local != "magma_block")
            tile = PickupTile::Full;
        terrainCache.emplace(pos, tile);
        return tile;
    };
    const NavPoint position{feet.x, feet.y, feet.z};
    const PlanCell here{static_cast<int>(std::floor(feet.x)),
                        static_cast<int>(std::ceil(feet.y - .12)),
                        static_cast<int>(std::floor(feet.z))};
    const PlanCell ore{target_.x, target_.y, target_.z};
    const auto distance = [&](PlanCell p) {
        return std::hypot(feet.x - p.x - .5, feet.z - p.z - .5);
    };
    const auto walkTo = [&](PlanCell p, bool jump) {
        const bedrock::Vec3 eye{feet.x, feet.y + 1.62f, feet.z};
        const bool facing = faceToward(eye, {p.x + .5f, eye.y, p.z + .5f}, 5.f);
        pressKey(kKeyW, facing && distance(p) > .16);
        pressKey(kKeySpace, facing && jump);
    };
    const auto owned = [&](PlanCell p) {
        const BlockPos3 pos{p.x, p.y, p.z};
        const auto it = placedPillars_.find(pos);
        return it != placedPillars_.end() && !collectDigFailed_.count(pos) &&
               it->second == localPart(blockAt(pos));
    };
    if (collectDig_) {
        stop();
        const auto cell = *collectDig_;
        const PlanCell floor{cell.x, cell.y, cell.z};
        const PlanCell stand{cell.x, cell.y + 1, cell.z};
        if (read(floor) == PickupTile::Air) {
            placedPillars_.erase(cell);
        } else if (distance(stand) < .24 && pickupLanded(position, stand, read) &&
                   pickupUndercut(stand, read, owned) && digCell(cell, feet, true) == 0) {
            return;
        } else {
            collectDigFailed_.insert(cell);
            note("collect: scaffold removal no longer safe or could not be completed");
        }
        setAttack(false);
        collectDig_.reset();
        resetCollectionSweep();
        return; // Wait for the lower stance before selecting another step.
    }
    if (collectRoute_.empty() && !pickupLanded(position, here, read)) {
        stop();
        return;
    }
    if (collectSweep_.empty())
        for (const auto p : pickupSweep(ore, here, read))
            collectSweep_.push_back({p.x, p.y, p.z});
    if (collectSweepIdx_ >= static_cast<int>(collectSweep_.size())) {
        stop();
        // If walking off is blocked, look for an exit after removing our
        // pillar one block at a time. Never dig the drop's natural floor.
        auto lower = here;
        const auto simulated = [&](PlanCell p) {
            return p.x == here.x && p.z == here.z && p.y >= lower.y && p.y < here.y
                       ? PickupTile::Air
                       : read(p);
        };
        while (const auto next = pickupUndercut(lower, simulated, owned)) {
            lower = *next;
            const auto candidates = pickupSweep(ore, lower, simulated);
            if (std::any_of(candidates.begin(), candidates.end(), [&](PlanCell p) {
                    return p.y <= lower.y && !pickupRoute(lower, p, simulated).empty();
                })) {
                if (distance(here) >= .20) {
                    if (++collectCandidateTicks_ <= 40) {
                        walkTo(here, false);
                        return;
                    }
                    break;
                }
                collectDig_ = BlockPos3{here.x, here.y - 1, here.z};
                note("collect: removing our pillar to reach a lower sweep");
                return;
            }
        }
        if (!collectRetryReported_) {
            note("collect: pickup unconfirmed; waiting and retrying the sweep");
            reply(collectDropId_ + " はまだ未回収。新しく掘らずに回収を続けるよ");
            collectRetryReported_ = true;
        }
        resetCollectionSweep();
        collectRetryTicks_ = 40;
        return;
    }
    if (collectRoute_.empty()) {
        const auto goal = collectSweep_[collectSweepIdx_];
        collectRoute_ = pickupRoute(here, {goal.x, goal.y, goal.z}, read);
        collectRouteIdx_ = 0;
        if (collectRoute_.empty()) {
            skip();
            return;
        }
    }
    if (++collectCandidateTicks_ > 80) {
        skip();
        return;
    }
    if (collectRouteIdx_ >= collectRoute_.size()) {
        stop();
        if (++collectDwell_ >= 8)
            skip();
        return;
    }
    const auto next = collectRoute_[collectRouteIdx_];
    if (distance(next) < .20 && pickupLanded(position, next, read)) {
        stop();
        if (++collectLandingTicks_ >= 2) {
            ++collectRouteIdx_;
            collectCandidateTicks_ = collectLandingTicks_ = 0;
        }
        return;
    }
    collectLandingTicks_ = 0;
    if (!collectRouteIdx_) {
        if (!(next == here) || !pickupLanded(position, next, read)) {
            skip();
            return;
        }
        walkTo(next, false);
        return;
    }
    const auto from = collectRoute_[collectRouteIdx_ - 1];
    const double lateral = from.x == next.x ? std::abs(feet.x - from.x - .5)
                                            : std::abs(feet.z - from.z - .5);
    const bool inCorridor = (here.x == from.x && here.z == from.z) ||
                            (here.x == next.x && here.z == next.z);
    if (!inCorridor || lateral >= .20 || feet.y < next.y - .62 || !pickupEdge(from, next, read)) {
        skip();
        return;
    }
    if (collectCandidateTicks_ == 1 && next.y < from.y)
        note("collect: descending " + std::to_string(from.y - next.y) + " to " +
             std::to_string(next.x) + "," + std::to_string(next.y) + "," + std::to_string(next.z));
    walkTo(next, next.y > from.y && next.y - feet.y > .60);
}

} // namespace automine::client
