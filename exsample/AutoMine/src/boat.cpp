#include "auto_mine.hpp"

namespace automine::client {

int AutoMineMod::boatSlot() {
    const auto slots = hotbarItems();
    for (int i = 0; i < 9; ++i) {
        const auto &it = slots[static_cast<std::size_t>(i)];
        if (it.count && contains(localPart(it.id), "_boat"))
            return i;
    }
    return -1;
}

std::optional<AutoMineMod::MobInfo> AutoMineMod::findBoat(const bedrock::sdk::Vec3Value &feet, double maxDist,
                                                          std::uint32_t onlyEid, bool freshOnly) {
    if (!actorsReady_)
        return std::nullopt;
    const auto snap = actorSamples();
    if (!snap)
        return std::nullopt;
    void *self = world_.localPlayer(client_.load());
    std::optional<MobInfo> best, bestTyped;
    for (const auto &sample : *snap) {
        void *a = reinterpret_cast<void *>(sample.actorKey);
        if (a == self)
            continue;
        const auto type = actorTypeOf(a);
        if (!type || *type == 64 || isHostileType(*type))
            continue;
        const std::uint32_t eid = sample.entityId;
        if (onlyEid && eid != onlyEid)
            continue;
        if (freshOnly && eid &&
            std::find(preBoatEids_.begin(), preBoatEids_.end(), eid) != preBoatEids_.end())
            continue;
        bedrock::sdk::ActorBoundsValue b{};
        if (const auto bounds = actors_.bounds(a))
            b = *bounds;
        else
            continue;
        const float w = b.maximum.x - b.minimum.x, h = b.maximum.y - b.minimum.y;
        if (*type != 90 && (w < 1.2f || w > 1.7f || h > 1.0f))
            continue;
        MobInfo m;
        m.type = *type;
        m.eid = eid;
        m.hasBounds = true;
        m.bmin = b.minimum;
        m.bmax = b.maximum;
        m.center = {(b.minimum.x + b.maximum.x) * .5f, (b.minimum.y + b.maximum.y) * .5f,
                    (b.minimum.z + b.maximum.z) * .5f};
        m.dist = std::hypot(m.center.x - feet.x, m.center.z - feet.z);
        if (m.dist > maxDist)
            continue;
        if (*type == 90) {
            if (!bestTyped || m.dist < bestTyped->dist)
                bestTyped = m;
        } else if (!best || m.dist < best->dist)
            best = m;
    }
    return bestTyped ? bestTyped : best;
}

void AutoMineMod::enterBoat() {
    releaseInputs();
    phase_ = Phase::Boat;
    boatStep_ = BoatStep::Deploy;
    boatTicks_ = 0;
    boatStepTicks_ = 0;
    boatPulse_ = 0;
    boatCd_ = 0;
    boatLockTicks_ = 0;
    boatTriedFor_ = target_;
    boatEid_ = 0;
    preBoatEids_.clear();
    boatGlueTicks_ = 0;
    boatProbeTicks_ = 0;
    if (actorsReady_) {
        if (auto snap = actorSamples()) {
            void *self = world_.localPlayer(client_.load());
            for (const auto &sample : *snap) {
                void *a = reinterpret_cast<void *>(sample.actorKey);
                if (a == self)
                    continue;
                const auto type = actorTypeOf(a);
                if (!type || *type != 90)
                    continue;
                const std::uint32_t eid = sample.entityId;
                if (eid)
                    preBoatEids_.push_back(eid);
            }
        }
    }
    note("boat: deploying for the crossing");
    reply("ボートで渡るね");
}

void AutoMineMod::giveUpBoat() {
    releaseInputs();
    note("boat: gave up; swimming instead");
    phase_ = Phase::Navigate;
    navTicks_ = 0;
}

void AutoMineMod::tickBoat(const bedrock::sdk::Vec3Value &feet) {
    ++boatTicks_;
    ++boatStepTicks_;
    const bedrock::sdk::Vec3Value eye{feet.x, feet.y + 1.62f, feet.z};
    const bedrock::sdk::Vec3Value center{target_.x + .5f, target_.y + .5f, target_.z + .5f};
    const double distT = std::hypot(center.x - feet.x, center.z - feet.z);
    if (!hasTarget_ || localPart(blockAt(target_)) != targetId_) {
        if (boatStep_ == BoatStep::Ride) {
            boatStep_ = BoatStep::Dismount;
            boatStepTicks_ = 0;
        } else {
            releaseInputs();
            phase_ = Phase::Scan;
            scanCursor_ = 0;
            bestDist2_ = 1e18;
        }
        if (boatStep_ != BoatStep::Dismount)
            return;
    }
    if (boatPulse_ > 0 && --boatPulse_ == 0)
        setUse(false);
    if (boatCd_ > 0)
        --boatCd_;
    switch (boatStep_) {
    case BoatStep::Deploy: {
        selectSlot(boatSlot());
        const double hl = std::max(0.001, distT);
        const bedrock::sdk::Vec3Value aim{feet.x + static_cast<float>((center.x - feet.x) / hl * 2.5),
                                feet.y + .4f,
                                feet.z + static_cast<float>((center.z - feet.z) / hl * 2.5)};
        faceToward(eye, aim, 4.f);
        pressKey(kKeySpace, true); // stay at the surface while placing
        if (auto boat = findBoat(feet, 5.0, 0, true)) {
            boatEid_ = boat->eid;
            note("boat: placed (actor type=" + std::to_string(boat->type) + ")");
            boatStep_ = BoatStep::Board;
            boatStepTicks_ = 0;
            break;
        }
        if (boatCd_ == 0 && boatStepTicks_ > 10) {
            setUse(true);
            boatPulse_ = 3;
            boatCd_ = 14;
        }
        if (boatStepTicks_ > 20 * 8)
            giveUpBoat();
        break;
    }
    case BoatStep::Board: {
        auto boat = findBoat(feet, 6.0, boatEid_);
        if (!boat) {
            if (boatStepTicks_ > 20 * 6)
                giveUpBoat();
            break;
        }
        boatCenter_ = boat->center;
        const double d = std::hypot(boat->center.x - feet.x, boat->center.z - feet.z);
        // Riding locks the player onto the boat at deck height;
        // swimming alongside keeps the feet well below the centre.
        if (d < 1.3 && feet.y > boat->center.y - .2f) {
            if (++boatLockTicks_ >= 6) {
                boatStep_ = BoatStep::Ride;
                boatStepTicks_ = 0;
                pressKey(kKeyW, false);
                pressKey(kKeySpace, false);
                note("boat: boarded");
                break;
            }
        } else
            boatLockTicks_ = 0;
        if (boatProbeTicks_ > 0) {
            faceAim(eye, {center.x, eye.y, center.z}, 6.f, .3f);
            pressKey(kKeyW, true);
            pressKey(kKeySpace, false);
            if (d > 1.6) { // separated without riding: resume Board
                boatProbeTicks_ = 0;
                boatGlueTicks_ = 0;
                pressKey(kKeyW, false);
            } else if (++boatProbeTicks_ > 40) {
                const double movedB = std::hypot(boat->center.x - boatProbeStart_.x,
                                                 boat->center.z - boatProbeStart_.z);
                pressKey(kKeyW, false);
                if (movedB > 2.5) {
                    boatStep_ = BoatStep::Ride;
                    boatStepTicks_ = 0;
                    note("boat: boarded (probe: hull carried player " + std::to_string(movedB) +
                         " blocks)");
                    break;
                }
                note("boat: probe failed (hull moved " + std::to_string(movedB) + "); not riding");
                giveUpBoat();
            }
            break;
        }
        if (d < 0.8) {
            pressKey(kKeyW, false);
            pressKey(kKeyS, false);
            pressKey(kKeySpace, true);
            if (++boatGlueTicks_ >= 12) {
                boatProbeTicks_ = 1;
                boatProbeStart_ = boat->center;
                note("boat: glued to hull; probing ride state");
            }
            break;
        }
        boatGlueTicks_ = 0;
        faceAim(eye, {boat->center.x, boat->center.y + .35f, boat->center.z}, 4.f, .3f);
        const double reach = distToMob(eye, *boat);
        const bool submerged = headInWater(feet);
        if (submerged) {
            pressKey(kKeyW, false);
            pressKey(kKeyS, false);
            pressKey(kKeySpace, true);
        } else if (reach > 3.0) {
            pressKey(kKeyS, false);
            pressKey(kKeyW, true);
            pressKey(kKeySpace, true);
        } else if (reach < 1.5) {
            pressKey(kKeyW, false);
            pressKey(kKeyS, true);
            pressKey(kKeySpace, true);
        } else {
            pressKey(kKeyW, false);
            pressKey(kKeyS, false);
            pressKey(kKeySpace, true);
            if (boatCd_ == 0) {
                setUse(true);
                boatPulse_ = 3;
                boatCd_ = 10;
            }
        }
        if (boatStepTicks_ > 20 * 20)
            giveUpBoat();
        break;
    }
    case BoatStep::Ride: {
        auto boat = findBoat(feet, 4.0, boatEid_);
        if (!boat) {
            boatStep_ = BoatStep::Board;
            boatStepTicks_ = 0;
            pressKey(kKeyW, false);
            break;
        }
        boatCenter_ = boat->center;
        if (distT <= 4.5) {
            boatStep_ = BoatStep::Dismount;
            boatStepTicks_ = 0;
            pressKey(kKeyW, false);
            break;
        }
        faceAim(eye, {center.x, eye.y, center.z}, 6.f, .3f);
        pressKey(kKeyW, true);
        if (boatTicks_ > 20 * 75) {
            boatStep_ = BoatStep::Dismount;
            boatStepTicks_ = 0;
            note("boat: ride timeout");
        }
        break;
    }
    case BoatStep::Dismount: {
        pressKey(kKeyW, false);
        pressKey(kKeySneak, true);
        if (boatStepTicks_ > 6) {
            pressKey(kKeySneak, false);
            phase_ = Phase::Navigate;
            pathFor_ = {INT_MIN, 0, 0};
            navTicks_ = 0;
            wasInWater_ = false;
            note("boat: dismounted");
        }
        break;
    }
    }
}

} // namespace automine::client
