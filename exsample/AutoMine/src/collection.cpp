#include "auto_mine.hpp"
#include "collection_geometry.hpp"

namespace automine::client {

void AutoMineMod::resetCollectionSweep() {
    // r18: no pickup ring/sweep.  Keep the normal A* path fields consistent.
    path_.clear();
    pathDig_.clear();
    pathBridge_.clear();
    pathParkour_.clear();
    pathPillar_.clear();
    pathIdx_ = 0;
    pathFor_ = {INT_MIN, 0, 0};
    pathPartial_ = false;
    jumpHoldTicks_ = 0;
}

void AutoMineMod::startCollection() {
    collectDropId_ = pickupItem_;
    collectBase_ = pickupBase_;
    collectStableTicks_ = collectAttemptTicks_ = collectNextScan_ = collectNextPlan_ = 0;
    collectArrivalReported_ = false;
    collectFailAttempts_ = 0;
    collectEntityValid_ = false;
    collectEntityPos_ = {};
    collectTarget_ = {INT_MIN, 0, 0};
    planBlocked_.clear();
    planBlockedFor_ = {INT_MIN, 0, 0};
    resetCollectionSweep();
    navTicks_ = stuckTicks_ = replans_ = 0;
    phase_ = Phase::Collect;
}

void AutoMineMod::tickCollect(bedrock::Vec3 feet) {
    setAttack(false);
    pressKey(kKeySneak, false);
    pressKey(kKeySprint, false);
    const auto stop = [&] {
        pressKey(kKeyW, false); pressKey(kKeyA, false); pressKey(kKeyD, false);
        pressKey(kKeyS, false); pressKey(kKeySpace, false);
    };

    if (++collectAttemptTicks_ >= 600) {
        stop();
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "collect: 30s timeout for %s; target=%.2f,%.2f,%.2f; abandoned",
            collectDropId_.c_str(), collectEntityPos_.x, collectEntityPos_.y, collectEntityPos_.z);
        note(buf);
        finishCollection(feet);
        return;
    }

    const int gain = collectDropId_.empty() ? 0 : invCountOf(collectDropId_) - collectBase_;
    if (collectDropId_.empty() || pickupConfirmed(gain, collectStableTicks_)) {
        if (!collectDropId_.empty())
            note("collect: verified " + collectDropId_ + " +" + std::to_string(gain));
        stop();
        finishCollection(feet);
        return;
    }

    // The picked-up actor can disappear before inventory verification settles.
    if (gain > 0) {
        stop();
        return;
    }

    if (collectAttemptTicks_ >= collectNextScan_) {
        collectNextScan_ = collectAttemptTicks_ + pickupScanInterval;
        // Keep only copied values between scans, never a live Actor pointer.
        if (const auto snap = actorsReady_ ? runtimeActors_.snapshot(level_.load()) : std::nullopt) {
            collectEntityValid_ = false;
            std::optional<PickupCandidateKey> best;
            void *self = world_.localPlayer(client_.load());
            const std::string wanted = localPart(collectDropId_);
            for (void *actor : *snap) {
                if (actor == self) continue;
                const auto type = actorTypeOf(actor);
                if (!couldBePickupActor(type)) continue;

                bedrock::Vec3 pos;
                if (!actorRegistry_.readPosition(actor, pos)) continue;
                const double dx = pos.x - feet.x, dy = pos.y - feet.y, dz = pos.z - feet.z;
                const double horizontal2 = dx * dx + dz * dz;
                if (horizontal2 > pickupScanRadius * pickupScanRadius ||
                    std::abs(dy) > pickupScanHeight)
                    continue;

                bedrock::Actor apiActor{game_, actor};
                const bedrock::ActorIdentity ident = apiActor.getIdentity();
                if (ident.isPlayer) continue;
                bool identityMatches = !ident.name.empty() &&
                                       localPart(ident.name) == wanted;

                if (!identityMatches) {
                    if (const auto main = apiActor.getMainHand()) {
                        identityMatches = !main->identifier.empty() &&
                                          localPart(main->identifier) == wanted;
                    }
                }

                // ItemActor has neither a name tag nor main-hand equipment
                // in the usual case. Its native type is enough to try pickup.
                const auto candidate =
                    pickupCandidateKey(identityMatches, type, horizontal2 + dy * dy);
                if (candidate && (!best || *candidate < *best)) {
                    best = candidate;
                    collectEntityPos_ = pos;
                    collectEntityValid_ = true;
                }
            }
        } else
            collectEntityValid_ = false;
    }

    if (!collectEntityValid_) {
        stop();
        if (!(collectAttemptTicks_ % 60))
            note("collect: drop not detected yet for " + collectDropId_ +
                 "; feet=" + std::to_string(feet.x) + "," + std::to_string(feet.y) +
                 "," + std::to_string(feet.z));
        collectTarget_ = {INT_MIN, 0, 0};
        collectArrivalReported_ = false;
        resetCollectionSweep();
        return;
    }

    const BlockPos3 target{
        static_cast<int>(std::floor(collectEntityPos_.x)),
        static_cast<int>(std::floor(collectEntityPos_.y)),
        static_cast<int>(std::floor(collectEntityPos_.z))};

    if (!(collectTarget_ == target)) {
        collectTarget_ = target;
        collectArrivalReported_ = false;
        target_ = target;
        targetId_ = collectDropId_;
        hasTarget_ = true;
        resetCollectionSweep();
        collectFailAttempts_ = 0;
        collectNextPlan_ = collectAttemptTicks_;
        navTicks_ = stuckTicks_ = replans_ = 0;
        note("collect: drop target " + collectDropId_ + " at " +
             std::to_string(collectEntityPos_.x) + "," + std::to_string(collectEntityPos_.y) +
             "," + std::to_string(collectEntityPos_.z) + " feet=" +
             std::to_string(feet.x) + "," + std::to_string(feet.y) + "," +
             std::to_string(feet.z));
    }

    const NavPoint item{collectEntityPos_.x, collectEntityPos_.y, collectEntityPos_.z};
    std::optional<PlanCell> endpoint;
    if (pathFor_ == target_ && !path_.empty() && !pathPartial_) {
        const auto end = path_.back();
        if (inPickupReach(pickupStancePoint({end.x, end.y, end.z}, item), item))
            endpoint = PlanCell{end.x, end.y, end.z};
        else
            resetCollectionSweep(); // item moved within its block cell
    }
    if (pickupArrived({feet.x, feet.y, feet.z}, item, endpoint)) {
        stop();
        if (!collectArrivalReported_) {
            collectArrivalReported_ = true;
            note("collect: arrived for " + collectDropId_ +
                 "; target=" + std::to_string(collectEntityPos_.x) + "," +
                 std::to_string(collectEntityPos_.y) + "," + std::to_string(collectEntityPos_.z) +
                 "; feet=" + std::to_string(feet.x) + "," + std::to_string(feet.y) + "," +
                 std::to_string(feet.z));
        }
        return;
    }
    collectArrivalReported_ = false;

    if (replans_ >= 3) {
        stop();
        note("collect: walking route repeatedly blocked; abandoned");
        finishCollection(feet);
        return;
    }
    // Use the normal A* route and normal follow/movement executor.
    if (!(pathFor_ == target_) || path_.empty()) {
        if (collectAttemptTicks_ < collectNextPlan_) {
            stop();
            return;
        }
        if (!planPath(feet)) {
            ++collectFailAttempts_;
            stop();
            collectNextPlan_ = collectAttemptTicks_ + 10;
            note("collect: normal route unavailable attempt=" + std::to_string(collectFailAttempts_) +
                 " target=" + std::to_string(target.x) + "," + std::to_string(target.y) +
                 "," + std::to_string(target.z));
            if (collectFailAttempts_ >= 3)
                finishCollection(feet);
            return;
        }
        navTicks_ = stuckTicks_ = 0;
    }
    tickFollowPath(feet);
}

} // namespace automine::client
