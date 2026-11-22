#include "auto_mine.hpp"
#include <nlohmann/json.hpp>

namespace automine::client {

bool AutoMineMod::isHostileType(std::uint32_t t) {
    switch (t) {
    case 199456:  // zombie
    case 1116962: // skeleton
    case 264995:  // spider
    case 2849:    // creeper
    case 199534:  // drowned (zombie family)
    case 199540:  // zombie variant (zombie family)
        return true;
    default:
        return false;
    }
}

bool AutoMineMod::isCreeperType(std::uint32_t t) {
    return t == 2849;
}

std::optional<AutoMineMod::MobInfo>
AutoMineMod::scanHostile(const bedrock::sdk::Vec3Value &feet, double maxDist, std::uint32_t onlyEid) {
    if (!actorsReady_)
        return std::nullopt;
    const auto snap = actorSamples();
    if (!snap)
        return std::nullopt;
    void *self = world_.localPlayer(client_.load());
    std::optional<MobInfo> best;
    for (const auto &sample : *snap) {
        void *a = reinterpret_cast<void *>(sample.actorKey);
        if (a == self)
            continue;
        const auto type = actorTypeOf(a);
        if (!type || !isHostileType(*type))
            continue;
        const std::uint32_t eid = sample.entityId;
        if (onlyEid && eid != onlyEid)
            continue;
        const auto posOpt = actors_.position(a);
        if (!posOpt)
            continue;
        const bedrock::sdk::Vec3Value pos = *posOpt;
        MobInfo m;
        m.eid = eid;
        m.type = *type;
        bedrock::sdk::ActorBoundsValue b{};
        if (const auto bounds = actors_.bounds(a)) {
            b = *bounds;
            m.hasBounds = true;
            m.bmin = b.minimum;
            m.bmax = b.maximum;
            m.center = {(b.minimum.x + b.maximum.x) * .5f, (b.minimum.y + b.maximum.y) * .5f,
                        (b.minimum.z + b.maximum.z) * .5f};
        } else {
            m.center = {pos.x, pos.y + .9f, pos.z};
        }
        m.dist = std::sqrt((m.center.x - feet.x) * (m.center.x - feet.x) +
                           (m.center.y - feet.y) * (m.center.y - feet.y) +
                           (m.center.z - feet.z) * (m.center.z - feet.z));
        if (!onlyEid && combatIgnore_.count(eid))
            continue; // gave up on it recently
        if (m.dist <= maxDist && (!best || m.dist < best->dist))
            best = m;
    }
    return best;
}

bool AutoMineMod::isFoodAnimalType(std::uint32_t t) {
    switch (t) {
    case 4874: // chicken (0.30x0.40)
    case 4875: // cow (0.90x1.30)
    case 4876: // pig (0.90x0.90)
    case 4877: // sheep (0.90x1.30)
        return true;
    default:
        return false;
    }
}

const char *AutoMineMod::rawMeatOf(std::uint32_t t) {
    switch (t) {
    case 4874:
        return "chicken";
    case 4875:
        return "beef";
    case 4876:
        return "porkchop";
    case 4877:
        return "mutton";
    default:
        return "";
    }
}

std::string AutoMineMod::cookedMeatOf(const std::string &raw) {
    if (raw == "beef")
        return "cooked_beef";
    if (raw == "porkchop")
        return "cooked_porkchop";
    if (raw == "chicken")
        return "cooked_chicken";
    if (raw == "mutton")
        return "cooked_mutton";
    return "";
}

std::optional<AutoMineMod::MobInfo>
AutoMineMod::scanPassive(const bedrock::sdk::Vec3Value &feet, double maxDist, std::uint32_t onlyEid) {
    if (!actorsReady_)
        return std::nullopt;
    const auto snap = actorSamples();
    if (!snap)
        return std::nullopt;
    void *self = world_.localPlayer(client_.load());
    std::optional<MobInfo> best;
    for (const auto &sample : *snap) {
        void *a = reinterpret_cast<void *>(sample.actorKey);
        if (a == self)
            continue;
        const auto type = actorTypeOf(a);
        if (!type || !isFoodAnimalType(*type))
            continue;
        const std::uint32_t eid = sample.entityId;
        if (onlyEid && eid != onlyEid)
            continue;
        const auto posOpt = actors_.position(a);
        if (!posOpt)
            continue;
        const bedrock::sdk::Vec3Value pos = *posOpt;
        MobInfo m;
        m.eid = eid;
        m.type = *type;
        bedrock::sdk::ActorBoundsValue b{};
        if (const auto bounds = actors_.bounds(a)) {
            b = *bounds;
            m.hasBounds = true;
            m.bmin = b.minimum;
            m.bmax = b.maximum;
            m.center = {(b.minimum.x + b.maximum.x) * .5f, (b.minimum.y + b.maximum.y) * .5f,
                        (b.minimum.z + b.maximum.z) * .5f};
        } else {
            m.center = {pos.x, pos.y + .9f, pos.z};
        }
        m.dist = std::sqrt((m.center.x - feet.x) * (m.center.x - feet.x) +
                           (m.center.y - feet.y) * (m.center.y - feet.y) +
                           (m.center.z - feet.z) * (m.center.z - feet.z));
        if (m.dist <= maxDist && (!best || m.dist < best->dist))
            best = m;
    }
    return best;
}

double AutoMineMod::distToMob(const bedrock::sdk::Vec3Value &p, const MobInfo &m) {
    if (!m.hasBounds) {
        return std::sqrt((m.center.x - p.x) * (m.center.x - p.x) +
                         (m.center.y - p.y) * (m.center.y - p.y) +
                         (m.center.z - p.z) * (m.center.z - p.z));
    }
    const double dx = p.x < m.bmin.x ? m.bmin.x - p.x : (p.x > m.bmax.x ? p.x - m.bmax.x : 0);
    const double dy = p.y < m.bmin.y ? m.bmin.y - p.y : (p.y > m.bmax.y ? p.y - m.bmax.y : 0);
    const double dz = p.z < m.bmin.z ? m.bmin.z - p.z : (p.z > m.bmax.z ? p.z - m.bmax.z : 0);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

int AutoMineMod::weaponTierOf(const std::string &lp) {
    if (lp.rfind("netherite_", 0) == 0)
        return 5;
    if (lp.rfind("diamond_", 0) == 0)
        return 4;
    if (lp.rfind("iron_", 0) == 0)
        return 3;
    if (lp.rfind("stone_", 0) == 0)
        return 2;
    if (lp.rfind("golden_", 0) == 0)
        return 1;
    if (lp.rfind("wooden_", 0) == 0)
        return 1;
    return 0;
}

AutoMineMod::Weapon AutoMineMod::bestMelee() {
    Weapon best;
    const auto slots = hotbarItems();
    for (int i = 0; i < 9; ++i) {
        const auto &it = slots[static_cast<std::size_t>(i)];
        if (!it.count || it.id.empty())
            continue;
        const auto lp = localPart(it.id);
        int kind = 0;
        if (lp.size() > 6 && lp.compare(lp.size() - 6, 6, "_sword") == 0)
            kind = 1;
        // "pickaxe" cannot false-match: it ends in "kaxe", not "_axe".
        else if (lp.size() > 4 && lp.compare(lp.size() - 4, 4, "_axe") == 0)
            kind = 2;
        if (!kind)
            continue;
        const int tier = weaponTierOf(lp);
        // Swords win ties (faster swing); then higher tier.
        if (best.slot < 0 || tier > best.tier || (tier == best.tier && kind < best.kind)) {
            best.slot = i;
            best.kind = kind;
            best.tier = tier;
        }
    }
    return best;
}

int AutoMineMod::bowSlot() {
    const auto slots = hotbarItems();
    for (int i = 0; i < 9; ++i) {
        const auto &it = slots[static_cast<std::size_t>(i)];
        if (it.count && localPart(it.id) == "bow")
            return i;
    }
    return -1;
}

bool AutoMineMod::hasArrows() {
    for (const auto &it : hotbarItems())
        if (it.count && localPart(it.id) == "arrow")
            return true;
    return false;
}

void AutoMineMod::enterCombat(const MobInfo &mob) {
    releaseInputs();
    setAttack(false);
    resumePhase_ = phase_;
    phase_ = Phase::Combat;
    combatEid_ = mob.eid;
    combatType_ = mob.type;
    combatTicks_ = 0;
    attackCd_ = 0;
    attackPulse_ = 0;
    retreatTicks_ = 0;
    bowDraw_ = 0;
    bowCd_ = 0;
    landedHits_ = 0;
    lastEngageTick_ = 0;
    clearPath();
    const int bow = bowSlot();
    const Weapon melee = bestMelee();
    if (isCreeperType(mob.type) && bow >= 0 && hasArrows()) {
        combatMode_ = CombatMode::Bow;
        selectSlot(bow);
    } else if (melee.slot >= 0 || !isCreeperType(mob.type)) {
        combatMode_ = CombatMode::Melee;
        if (melee.slot >= 0)
            selectSlot(melee.slot);
    } else {
        combatMode_ = CombatMode::Flee; // creeper, no bow, no weapon
    }
    note("combat: start type=" + std::to_string(mob.type) +
         " mode=" + std::to_string(static_cast<int>(combatMode_)));
    reply("敵が来た！応戦するよ");
}

void AutoMineMod::exitCombat(bool won) {
    releaseInputs();
    setAttack(false);
    setUse(false);
    if (!won && combatEid_) // unreachable or timed out: don't retrigger at once
        combatIgnore_[combatEid_] = tickCount_ + 20 * 60;
    if (won && landedHits_ > 0)
        reply("やっつけたよ");
    note("combat: end won=" + std::to_string(won ? 1 : 0));
    combatEid_ = 0;
    if (resumePhase_ == Phase::Collect) {
        // The ore is already gone; its continued absence is expected.
        resetCollectionSweep();
        phase_ = Phase::Collect;
    } else if ((resumePhase_ == Phase::Navigate || resumePhase_ == Phase::Mine ||
                resumePhase_ == Phase::NeedTool) &&
        hasTarget_ && !targetId_.empty() && localPart(blockAt(target_)) == targetId_) {
        phase_ = resumePhase_;
        pathFor_ = {INT_MIN, 0, 0};
        replans_ = 0;
    } else if (resumePhase_ == Phase::Idle) {
        phase_ = Phase::Idle; // idle self-defense: go back to waiting
    } else {
        phase_ = Phase::Scan;
        scanCursor_ = 0;
        bestDist2_ = 1e18;
    }
    resumePhase_ = Phase::Scan;
}

void AutoMineMod::tickCombat(const bedrock::sdk::Vec3Value &feet) {
    ++combatTicks_;
    if (combatTicks_ > 20 * 90) {
        exitCombat(false);
        return;
    } // 90 s cap
    if (combatTicks_ - lastEngageTick_ > 20 * 25) {
        exitCombat(false);
        return;
    }
    auto mob = scanHostile(feet, 40.0, combatEid_);
    if (!mob) {
        // Target died or left: retarget to any other nearby hostile,
        // otherwise the fight is over.
        if (auto other = scanHostile(feet, 14.0)) {
            combatEid_ = other->eid;
            combatType_ = other->type;
            mob = other;
            note("combat: retarget type=" + std::to_string(other->type));
        } else {
            exitCombat(true);
            return;
        }
    }
    const bedrock::sdk::Vec3Value eye{feet.x, feet.y + 1.62f, feet.z};
    BlockPos3 hitCell{};
    const BlockPos3 mobCell{static_cast<int>(std::floor(mob->center.x)),
                            static_cast<int>(std::floor(mob->center.y)),
                            static_cast<int>(std::floor(mob->center.z))};
    const bool los = lineOfSight(eye, mobCell, hitCell);
    const double reach = distToMob(eye, *mob);
    const bool wet = inWaterNow(feet);
    if (attackCd_ > 0)
        --attackCd_;
    if (attackPulse_ > 0 && --attackPulse_ == 0) {
        setAttack(false);
        if (retreatAfterHit_) {
            retreatAfterHit_ = false;
            retreatTicks_ = 10;
        }
    }
    if (combatMode_ == CombatMode::Flee) {
        // Run directly away from the mob until it is far enough.
        lastEngageTick_ = combatTicks_; // fleeing is the engagement
        bedrock::sdk::Vec3Value away{feet.x * 2 - mob->center.x, mob->center.y, feet.z * 2 - mob->center.z};
        faceAim(eye, away, 6.f, .4f);
        pressKey(kKeyW, true);
        if (wet)
            pressKey(kKeySpace, true);
        if (mob->dist > 16.0)
            exitCombat(true);
        return;
    }
    if (combatMode_ == CombatMode::Bow) {
        const double d = std::sqrt((mob->center.x - eye.x) * (mob->center.x - eye.x) +
                                   (mob->center.y - eye.y) * (mob->center.y - eye.y) +
                                   (mob->center.z - eye.z) * (mob->center.z - eye.z));
        if (bowDraw_ > 0) {
            // Drawing: hold still, track the mob, release at full draw.
            pressKey(kKeyW, false);
            pressKey(kKeyS, false);
            if (wet)
                pressKey(kKeySpace, true);
            bedrock::sdk::Vec3Value aim = mob->center;
            aim.y += static_cast<float>(d * 0.02);
            faceToward(eye, aim, 2.5f);
            if (!los) {
                setUse(false);
                bowDraw_ = 0;
                bowCd_ = 6;
            } else if (--bowDraw_ == 0) {
                setUse(false);
                bowCd_ = 9;
                ++landedHits_;
                lastEngageTick_ = combatTicks_;
            }
            return;
        }
        if (bowCd_ > 0)
            --bowCd_;
        faceAim(eye, mob->center, 4.f, .35f);
        if (!los || d > 14.0) { // close in until the shot is clear
            pressKey(kKeyS, false);
            pressKey(kKeyW, true);
            if (wet)
                pressKey(kKeySpace, true);
        } else if (d < 7.0) { // too close: back off while facing it
            pressKey(kKeyW, false);
            pressKey(kKeyS, true);
        } else {
            pressKey(kKeyW, false);
            pressKey(kKeyS, false);
            if (bowCd_ == 0) {
                setUse(true);
                bowDraw_ = 22;
            }
        }
        return;
    }
    // Melee.
    if (retreatTicks_ > 0) {
        --retreatTicks_;
        faceAim(eye, mob->center, 5.f, .4f);
        pressKey(kKeyW, false);
        pressKey(kKeyS, true);
        if (wet)
            pressKey(kKeySpace, true);
        return;
    }
    pressKey(kKeyS, false);
    if (reach > 2.9 || !los) {
        faceAim(eye, mob->center, 5.f, .35f);
        pressKey(kKeyW, true);
        if (wet || mob->center.y > feet.y + .6f)
            pressKey(kKeySpace, true);
        else
            pressKey(kKeySpace, false);
        return;
    }
    pressKey(kKeyW, false);
    pressKey(kKeySpace, wet); // stay at the surface while striking
    faceToward(eye, mob->center, 3.f);
    if (attackCd_ == 0) {
        setAttack(true);
        attackPulse_ = 2;
        attackCd_ = 11;
        ++landedHits_;
        lastEngageTick_ = combatTicks_;
        if (isCreeperType(combatType_))
            retreatAfterHit_ = true;
    }
}

bool AutoMineMod::loadActors(const bedrock::sdk::Profile &profile,
                             const bedrock::sdk::SymbolResolver &resolver,
                             std::string &failure) {
    try {
        const auto root = nlohmann::json::parse(profile.json());
        const auto &at = root.at("layouts").at("Actor").at("actor_type");
        // Profile data stores offsets as hex strings ("0x0") or numbers;
        // the native layout parsers accepted both, so do the same here.
        auto layoutNumber = [](const nlohmann::json &value) -> std::uint64_t {
            if (value.is_string())
                return std::stoull(value.get<std::string>(), nullptr, 0);
            return value.get<std::uint64_t>();
        };
        return actors_.configure(profile.json(),
                                 static_cast<std::size_t>(layoutNumber(at.at("value_offset"))),
                                 static_cast<std::uint32_t>(layoutNumber(at.at("player_type_id"))),
                                 resolver, failure);
    } catch (const std::exception &e) {
        failure = e.what();
        return false;
    }
}

std::optional<std::uint32_t> AutoMineMod::actorTypeOf(void *actor) {
    return actors_.actorType(actor);
}

std::optional<std::vector<bedrock::sdk::ActorSample>> AutoMineMod::actorSamples() {
    std::vector<bedrock::sdk::ActorSample> samples;
    std::size_t cursor = 0;
    std::string error;
    // ActorFacade::capture takes the LOCAL PLAYER actor (its registry
    // context anchors the population read), not the level pointer the
    // old native snapshot API used.
    void *local = world_.localPlayer(client_.load());
    if (!local)
        return std::nullopt;
    if (!actors_.capture(local, false, cursor, samples, error))
        return std::nullopt;
    return samples;
}

void AutoMineMod::probeActors(const bedrock::sdk::Vec3Value &feet) {
    if (!actorsReady_) {
        note("probe: actor access not ready");
        return;
    }
    const auto snap = actorSamples();
    if (!snap) {
        note("probe: snapshot failed");
        return;
    }
    note("probe: raw actors=" + std::to_string(snap->size()));
    void *self = world_.localPlayer(client_.load());
    int n = 0;
    for (const auto &sample : *snap) {
        void *a = reinterpret_cast<void *>(sample.actorKey);
        const auto posOpt = actors_.position(a);
        if (!posOpt)
            continue;
        const bedrock::sdk::Vec3Value pos = *posOpt;
        const double d = std::hypot(pos.x - feet.x, pos.z - feet.z);
        if (d > 40)
            continue;
        ++n;
        bedrock::sdk::ActorBoundsValue b{};
        const auto bounds = actors_.bounds(a);
        const bool hasB = bounds.has_value();
        if (bounds)
            b = *bounds;
        const std::uint32_t eid = sample.entityId;
        const auto type = actorTypeOf(a);
        const bedrock::sdk::ActorIdentityValue ident = access_.actorIdentity(a);
        char buf[640];
        std::snprintf(buf, sizeof(buf),
                      "actor d=%.1f self=%d type=%d eid=%u bounds=%.2fx%.2f pos=%.1f,%.1f,%.1f "
                      "name='%s' tag='%s' player=%d",
                      d, a == self ? 1 : 0, type ? static_cast<int>(*type) : -1, eid,
                      hasB ? (b.maximum.x - b.minimum.x) : -1.f,
                      hasB ? (b.maximum.y - b.minimum.y) : -1.f, pos.x, pos.y, pos.z,
                      ident.name.c_str(), ident.nameTag.c_str(), ident.isPlayer ? 1 : 0);
        note(buf);
    }
    reply("probe: " + std::to_string(n) + " actors logged");
}

} // namespace automine::client
