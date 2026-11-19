#include "auto_mine.hpp"
#include "diagnostics.hpp"

namespace automine::client {

bedrock::Keyboard AutoMineMod::keyboard() {
    return game_.localPlayer().getKeyboard();
}

bedrock::Mouse AutoMineMod::mouse() {
    return game_.localPlayer().getMouse();
}

void AutoMineMod::installMouseCapture(const bedrock::Resolver &resolver) {
    auto res = resolver.resolveRvaForSharedHook("MouseDevice::feed");
    if (!res) {
        note("mouse capture: MouseDevice::feed unresolved");
        return;
    }
    auto *target = reinterpret_cast<void *>(res->address);
    if (!BDS_HookSubscribe(BDS_HOOK_MOUSE_FEED, target, &onMouseCapture, this, &mouseToken_)) {
        note("mouse capture: subscribe failed");
        return;
    }
    note("mouse capture: subscribed");
}

void BDS_HOOK_CALL AutoMineMod::onMouseCapture(void *user, void *raw, BDS_HookNext next,
                                               void *frame) noexcept {
    auto &self = *static_cast<AutoMineMod *>(user);
    try {
        std::lock_guard lock(self.mouseMutex_);
        self.lastMouse_ = *static_cast<BDS_MouseArgs *>(raw);
        self.haveMouse_ = true;
    } catch (...) {
    }
    next(frame, raw);
}

uintptr_t AutoMineMod::dispatchMouseSeh(void *args) {
    __try {
        return BDS_HookDispatch(BDS_HOOK_MOUSE_FEED, args);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool AutoMineMod::feedMouse(int action, int state, int dx, int dy) {
    crashrec::Section section("feedMouse");
    BDS_MouseArgs args;
    {
        std::lock_guard lock(mouseMutex_);
        if (!haveMouse_)
            return false;
        args = lastMouse_;
    }
    args.action = static_cast<std::int8_t>(action);
    args.state = static_cast<std::int8_t>(state);
    args.dx = static_cast<std::int16_t>(std::clamp(dx, -32768, 32767));
    args.dy = static_cast<std::int16_t>(std::clamp(dy, -32768, 32767));
    args.x = static_cast<std::int16_t>(std::clamp(static_cast<int>(args.x) + dx, -32768, 32767));
    args.y = static_cast<std::int16_t>(std::clamp(static_cast<int>(args.y) + dy, -32768, 32767));
    args.force_motionless_pointer &= BDS_MOUSE_FLAG_FORCE_MOTIONLESS;
    dispatchMouseSeh(&args);
    {
        std::lock_guard lock(mouseMutex_);
        lastMouse_.x = args.x;
        lastMouse_.y = args.y;
    }
    return true;
}

bool AutoMineMod::feedMove(int dx, int dy) {
    return feedMouse(0, 0, dx, dy);
}

bool AutoMineMod::feedButton(bool down) {
    return feedMouse(1, down ? 1 : 0, 0, 0);
}

bool AutoMineMod::feedUseButton(bool down) {
    return feedMouse(2, down ? 1 : 0, 0, 0);
}

void AutoMineMod::setUse(bool down) {
    if (independent_) {
        if (down && !useHeld_)
            usePulsePending_ = true;
        useHeld_ = down;
        return;
    }
    if (useHeld_ == down)
        return;
    if (feedUseButton(down) || mouse().setDown(bedrock::MouseButton::Right, down))
        useHeld_ = down;
}

void AutoMineMod::pressKey(unsigned key, bool down) {
    if (independent_) {
        const bool movement = key == kKeyW || key == kKeyA || key == kKeyS || key == kKeyD ||
                              key == kKeySpace || key == kKeySneak || key == kKeySprint;
        if (movement) {
            if (down)
                heldKeys_.insert(key);
            else
                heldKeys_.erase(key);
            return;
        }
    }
    auto kb = keyboard();
    if (down) {
        if (!heldKeys_.count(key) && kb.press(key))
            heldKeys_.insert(key);
    } else if (heldKeys_.erase(key))
        kb.release(key);
}

void AutoMineMod::setAttack(bool down) {
    if (independent_) {
        attackHeld_ = down;
        return;
    }
    if (attackHeld_ == down)
        return;
    if (feedButton(down) || mouse().setDown(bedrock::MouseButton::Left, down))
        attackHeld_ = down;
}

void AutoMineMod::releaseInputs() {
    if (parkourSprintInput_) {
        note("path: parkour input released (stop/pause/route reset)");
        parkourWaypoint_ = {INT_MIN, 0, 0}; // restart the runup after any interruption
    }
    parkourSprintInput_ = false;
    if (independent_) {
        blockActions_.cancelMiningOwnership();
        // UI pulses belong to the craft chain; movement never sends keys.
        for (unsigned key : {0x45u, 0x1Bu, 0x0Du})
            if (heldKeys_.erase(key)) {
                try {
                    keyboard().release(key);
                } catch (...) {
                }
            }
        heldKeys_.clear();
        attackHeld_ = false;
        useHeld_ = false;
        usePulsePending_ = false;
        controls_.clear();
        return;
    }
    for (auto key : {kKeyW, kKeyA, kKeyS, kKeyD, kKeySpace, kKeySneak, kKeySprint}) {
        if (heldKeys_.erase(key)) {
            try {
                keyboard().release(key);
            } catch (...) {
            }
        }
    }
    if (attackHeld_) {
        try {
            feedButton(false);
            mouse().setDown(bedrock::MouseButton::Left, false);
        } catch (...) {
        }
        attackHeld_ = false;
    }
    if (useHeld_) {
        try {
            feedUseButton(false);
            mouse().setDown(bedrock::MouseButton::Right, false);
        } catch (...) {
        }
        useHeld_ = false;
    }
}

bool AutoMineMod::independentHit(bedrock::Vec3 eye, BlockPos3 &cell, unsigned &face,
                                 bedrock::Vec3 &hit) {
    if (!independentAim_)
        return false;
    constexpr double rad = 3.141592653589793 / 180.0;
    const double yaw = independentYaw_ * rad, pitch = independentPitch_ * rad;
    const bedrock::Vec3 dir{static_cast<float>(-std::sin(yaw) * std::cos(pitch)),
                            static_cast<float>(-std::sin(pitch)),
                            static_cast<float>(std::cos(yaw) * std::cos(pitch))};
    BlockPos3 previous{static_cast<int>(std::floor(eye.x)), static_cast<int>(std::floor(eye.y)),
                       static_cast<int>(std::floor(eye.z))};
    for (double distance = .02; distance <= automine::miningReach + .1; distance += .02) {
        const bedrock::Vec3 point{eye.x + dir.x * static_cast<float>(distance),
                                  eye.y + dir.y * static_cast<float>(distance),
                                  eye.z + dir.z * static_cast<float>(distance)};
        cell = {static_cast<int>(std::floor(point.x)), static_cast<int>(std::floor(point.y)),
                static_cast<int>(std::floor(point.z))};
        const auto id = blockAt(cell);
        if (id.empty())
            return false;
        if (isSolidCell(id)) {
            face = cell.y < previous.y   ? 1u
                   : cell.y > previous.y ? 0u
                   : cell.x > previous.x ? 4u
                   : cell.x < previous.x ? 5u
                   : cell.z > previous.z ? 2u
                                         : 3u;
            // Snap to the actual entry face, avoiding a point inside the
            // support block being interpreted as the opposite face.
            float t = static_cast<float>(distance);
            if (face < 2 && std::abs(dir.y) > .0001f)
                t = (cell.y + (face == 1 ? 1.f : 0.f) - eye.y) / dir.y;
            if ((face == 4 || face == 5) && std::abs(dir.x) > .0001f)
                t = (cell.x + (face == 5 ? 1.f : 0.f) - eye.x) / dir.x;
            if ((face == 2 || face == 3) && std::abs(dir.z) > .0001f)
                t = (cell.z + (face == 3 ? 1.f : 0.f) - eye.z) / dir.z;
            hit = {eye.x + dir.x * t, eye.y + dir.y * t, eye.z + dir.z * t};
            return true;
        }
        previous = cell;
    }
    return false;
}

void AutoMineMod::flushIndependent() {
    if (!independent_)
        return;
    const bool active =
        independentReady_ && !stopped_.load() && !controls_.manualActive() && !automationScreenUp();
    bedrock::AutomationIntent intent;
    intent.active = active && (phase_ != Phase::Idle || !heldKeys_.empty());
    intent.forward =
        static_cast<float>(heldKeys_.count(kKeyW)) - static_cast<float>(heldKeys_.count(kKeyS));
    intent.left =
        static_cast<float>(heldKeys_.count(kKeyA)) - static_cast<float>(heldKeys_.count(kKeyD));
    intent.jump = heldKeys_.count(kKeySpace);
    intent.sneak = heldKeys_.count(kKeySneak);
    intent.sprint = heldKeys_.count(kKeySprint);
    intent.yaw = independentYaw_;
    intent.pitch = independentPitch_;
    intent.look = independentAim_;
    intent.sprintJump = parkourSprintInput_;
    const int inputLog = (intent.active ? 1 : 0) | (intent.sprintJump ? 2 : 0) |
        (intent.sprint ? 4 : 0) | (intent.jump ? 8 : 0);
    if ((intent.sprintJump || (parkourInputLog_ & 2)) &&
        (inputLog != parkourInputLog_ || tickCount_ % 4 == 0)) {
        const auto observed = controls_.observation();
        note("parkour input: stage=" + std::to_string(parkourStage_) +
             " active=" + std::to_string(intent.active) +
             " forward=" + std::to_string(intent.forward) +
             " sprint=" + std::to_string(intent.sprint) +
             " jump=" + std::to_string(intent.jump) +
             " sim_seq=" + std::to_string(observed.sequence) +
             " applied=" + std::to_string(observed.applied) +
             " sim_sprint_jump=" + std::to_string(observed.sprintJump) +
             " sim_forward=" + std::to_string(observed.forward) +
             " sim_left=" + std::to_string(observed.left) +
             " sim_yaw=" + std::to_string(observed.yaw) +
             " flags=" + std::to_string(observed.input) +
             " held=" + std::to_string(observed.held) +
             " sprint_mask=" + std::to_string(observed.sprintMask) +
             " sprint_calls=" + std::to_string(observed.sprintCalls));
    }
    parkourInputLog_ = inputLog;
    controls_.update(client_.load(), epoch_.load(), intent, snap_ ? snap_->rotation.yaw : 0.f);
    void *p = player();
    if (!p) {
        nativeMining_ = false;
        nativeUsing_ = false;
        return;
    }
    bedrock::Vec3 eye;
    BlockPos3 cell{};
    unsigned face{};
    bedrock::Vec3 hit{};
    const bool targetMine = phase_ == Phase::Mine;
    const bool ray = active && (!targetMine || mineAimTick_ == tickCount_) &&
                     eyePos(eye) && independentHit(eye, cell, face, hit);
    const bool entityAttack = phase_ == Phase::Combat || phase_ == Phase::Forage;
    const bool scaffoldDig = phase_ == Phase::Collect && collectDig_.has_value();
    if (nativeMining_ && (!active || !attackHeld_ || entityAttack ||
                          (targetMine && nativeMiningCell_ != target_) ||
                          (scaffoldDig && nativeMiningCell_ != *collectDig_))) {
        (void)blockActions_.stop(p,
                                 {nativeMiningCell_.x, nativeMiningCell_.y, nativeMiningCell_.z});
        nativeMining_ = false;
        nativeMiningMissTicks_ = 0;
    } else if (!targetMine && nativeMining_ && ray && !(cell == nativeMiningCell_)) {
        ++nativeMiningMissTicks_;
        if (nativeMiningMissTicks_ < 8)
            return; // keep the accepted target until the change is stable
        // A one-frame aim/ray flicker must not end the destroy session.
        // Only a sustained change to another block restarts/stops it.
        if (nativeMiningMissTicks_ >= 8) {
            note("mine: destroy target changed; stopping old target");
            (void)blockActions_.stop(
                p, {nativeMiningCell_.x, nativeMiningCell_.y, nativeMiningCell_.z});
            nativeMining_ = false;
            nativeMiningMissTicks_ = 0;
        }
    } else if (nativeMining_) {
        nativeMiningMissTicks_ = 0;
    }
    if (active && attackHeld_ && entityAttack && tickCount_ - lastNativeAttack_ >= 6) {
        const auto eid = phase_ == Phase::Combat ? combatEid_ : forageEid_;
        if (eid && actorsReady_)
            if (const auto actors = runtimeActors_.snapshot(level_.load()))
                for (void *actor : *actors) {
                    std::uint32_t candidate{};
                    if (actorRegistry_.readEntityId(actor, candidate) && candidate == eid) {
                        (void)blockActions_.attackActor(p, actor);
                        lastNativeAttack_ = tickCount_;
                        break;
                    }
                }
    } else if (active && attackHeld_ && ray && !entityAttack &&
               (!targetMine || cell == target_) && (!scaffoldDig || cell == *collectDig_)) {
        const bool pocketByHand = digHandAllowed_ && cell == digPos_ &&
                                  (phase_ == Phase::Craft || phase_ == Phase::Smelt || scaffoldDig);
        const bool toolReady = ensureTool(blockAt(cell));
        if (toolSyncSlot_ >= 0 || (!toolReady && !pocketByHand)) {
            setAttack(false);
            if (nativeMining_)
                (void)blockActions_.stop(
                    p, {nativeMiningCell_.x, nativeMiningCell_.y, nativeMiningCell_.z});
            nativeMining_ = false;
            return;
        }
        bool destroyed{};
        bedrock::BlockActionReport report;
        const bool beginCall = !nativeMining_;
        const bool ok = blockActions_.mine(p, {cell.x, cell.y, cell.z}, face, eye, hit, beginCall,
                                           destroyed, &report);
        if (ok) {
            float prog = 0;
            bedrock::BlockPos tgt{-1, -1, -1};
            if (blockActions_.destroyState(p, prog, tgt)) {
                destroyProgress_ = prog;
                destroyTarget_ = tgt;
                if (targetMine && tgt.x == cell.x && tgt.y == cell.y && tgt.z == cell.z &&
                    miningProgress_.observe(cell, tickCount_, prog) &&
                    progressGoal_ == (savedId_.empty() ? cell : savedTarget_))
                    progressTick_ = tickCount_;
                if ((targetMine && beginCall) || tickCount_ % 20 == 0) {
                    char pb[240];
                    std::snprintf(pb, sizeof(pb),
                                  "mine: destroy progress %.3f -> %.3f target %d,%d,%d (cell "
                                  "%d,%d,%d) idle_stops=%llu begin=%d",
                                  report.progressBefore, prog, tgt.x, tgt.y, tgt.z, cell.x, cell.y,
                                  cell.z,
                                  static_cast<unsigned long long>(report.idleStopsSuppressed),
                                  beginCall ? 1 : 0);
                    note(pb);
                }
            }
            if (nativeActionWarned_)
                note("independent mining: recovered: " + blockActions_.diagnostic(report));
            nativeMining_ = true;
            nativeMiningCell_ = cell;
            nativeActionWarned_ = false;
        } else if (!nativeActionWarned_ || tickCount_ - lastNativeActionWarn_ >= 100) {
            note("independent mining: " + blockActions_.diagnostic(report));
            nativeActionWarned_ = true;
            lastNativeActionWarn_ = tickCount_;
        }
    }
    if (nativeUsing_ && (!active || !useHeld_)) {
        (void)blockActions_.releaseItem(p);
        nativeUsing_ = false;
    }
    if (active && !targetMine && useHeld_ && !anyScreenUp()) {
        const bool heldItem = (phase_ == Phase::Combat && combatMode_ == CombatMode::Bow) ||
                              (phase_ == Phase::Forage && forageStage_ == ForageStage::Eat);
        if (heldItem && !nativeUsing_)
            nativeUsing_ = blockActions_.useItem(p);
        else if (!heldItem && ray && (usePulsePending_ || tickCount_ - lastNativeUse_ >= 4)) {
            const auto id = localPart(blockAt(cell));
            if (backgroundCraft_ && (contains(id, "crafting_table") || contains(id, "furnace"))) {
                if (!craft_.backgroundActive()) {
                    const auto kind =
                        contains(id, "crafting_table")
                            ? bedrock::NativeItemStackRequests::BackgroundKind::Table
                            : bedrock::NativeItemStackRequests::BackgroundKind::Furnace;
                    (void)craft_.backgroundUse(kind, {cell.x, cell.y, cell.z}, face, eye, hit);
                }
                lastNativeUse_ = tickCount_;
                usePulsePending_ = false;
                return;
            }
            bool accepted{};
            (void)world_.build(p, world_.gameMode(p), {cell.x, cell.y, cell.z}, face, eye, hit,
                               &accepted);
            lastNativeUse_ = tickCount_;
            usePulsePending_ = false;
        }
    }
}

void *AutoMineMod::player() {
    return world_.localPlayer(client_.load());
}

void *AutoMineMod::region() {
    return world_.region(client_.load());
}

bool AutoMineMod::feetPos(bedrock::Vec3 &out) {
    void *p = player();
    if (!p)
        return false;
    const auto &actors = world_.actors();
    void *shape = world_.component(p, actors.aabbShapeComponentHash);
    if (!shape && actors.aabbShapeComponentHashAlternate)
        shape = world_.component(p, actors.aabbShapeComponentHashAlternate);
    if (shape) {
        bedrock::Vec3 mn{}, mx{};
        if (world_.readField(shape, actors.aabbMinimumOffset, mn) &&
            world_.readField(shape, actors.aabbMaximumOffset, mx) && mx.y > mn.y &&
            mx.y - mn.y < 4.f) {
            out = {(mn.x + mx.x) * .5f, mn.y, (mn.z + mx.z) * .5f};
            return true;
        }
    }
    std::uint64_t rt{};
    if (!world_.pose(p, out, rt))
        return false;
    out.y -= 1.62f; // fallback: pose read is eye-anchored in most states
    return true;
}

bool AutoMineMod::eyePos(bedrock::Vec3 &out) {
    if (!feetPos(out))
        return false;
    out.y += 1.62f;
    return true;
}

std::string AutoMineMod::blockAt(BlockPos3 pos) {
    void *src = region();
    if (!src)
        return {};
    bedrock::BlockPos bp;
    bp.x = pos.x;
    bp.y = pos.y;
    bp.z = pos.z;
    auto cell = world_.getBlock(src, bp);
    return cell.identifier;
}

bool AutoMineMod::lineOfSight(bedrock::Vec3 eye, BlockPos3 targetCell, BlockPos3 &hit) {
    const auto miningHit =
        automine::miningPoint({eye.x, eye.y, eye.z}, {targetCell.x, targetCell.y, targetCell.z});
    const bedrock::Vec3 goal{static_cast<float>(miningHit.x), static_cast<float>(miningHit.y),
                             static_cast<float>(miningHit.z)};
    double dx = goal.x - eye.x, dy = goal.y - eye.y, dz = goal.z - eye.z;
    const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-6)
        return true;
    dx /= len;
    dy /= len;
    dz /= len;
    int cx = static_cast<int>(std::floor(eye.x));
    int cy = static_cast<int>(std::floor(eye.y));
    int cz = static_cast<int>(std::floor(eye.z));
    const int stepX = dx > 0 ? 1 : -1, stepY = dy > 0 ? 1 : -1, stepZ = dz > 0 ? 1 : -1;
    auto boundary = [](double pos, double dir) {
        if (dir == 0)
            return 1e30;
        const double cell = dir > 0 ? std::floor(pos) + 1.0 : std::floor(pos);
        return std::abs((cell - pos) / dir);
    };
    double tX = boundary(eye.x, dx), tY = boundary(eye.y, dy), tZ = boundary(eye.z, dz);
    const double dtX = dx == 0 ? 1e30 : std::abs(1.0 / dx);
    const double dtY = dy == 0 ? 1e30 : std::abs(1.0 / dy);
    const double dtZ = dz == 0 ? 1e30 : std::abs(1.0 / dz);
    double t = 0;
    for (int i = 0; i < 64 && t <= len; ++i) {
        if (tX < tY && tX < tZ) {
            cx += stepX;
            t = tX;
            tX += dtX;
        } else if (tY < tZ) {
            cy += stepY;
            t = tY;
            tY += dtY;
        } else {
            cz += stepZ;
            t = tZ;
            tZ += dtZ;
        }
        if (cx == targetCell.x && cy == targetCell.y && cz == targetCell.z)
            return true;
        const auto id = blockAt({cx, cy, cz});
        if (!id.empty() && !isAir(id) && !contains(localPart(id), "water") &&
            !contains(localPart(id), "lava") && !isPassableCell(id)) {
            hit = {cx, cy, cz};
            return false;
        }
    }
    return true; // reached the goal distance without entering the cell
}

bool AutoMineMod::rotation(float &yaw, float &pitch) {
    if (!snap_)
        return false;
    yaw = independent_ && independentAim_ ? independentYaw_ : snap_->rotation.yaw;
    pitch = independent_ && independentAim_ ? independentPitch_ : snap_->rotation.pitch;
    return true;
}

float AutoMineMod::wrapDeg(float deg) {
    while (deg > 180.f)
        deg -= 360.f;
    while (deg < -180.f)
        deg += 360.f;
    return deg;
}

bool AutoMineMod::faceToward(bedrock::Vec3 eye, bedrock::Vec3 target, float tolerance) {
    const double dx = target.x - eye.x, dy = target.y - eye.y, dz = target.z - eye.z;
    const double horiz = std::hypot(dx, dz);
    const float wantYaw = static_cast<float>(std::atan2(-dx, dz) * 180.0 / 3.141592653589793);
    const float wantPitch = static_cast<float>(std::atan2(-dy, horiz) * 180.0 / 3.141592653589793);
    if (independent_) {
        independentYaw_ = wantYaw;
        independentPitch_ = std::clamp(wantPitch, -89.f, 89.f);
        independentAim_ = true;
        return independentReady_;
    }
    float yaw{}, pitch{};
    if (!rotation(yaw, pitch))
        return false;
    if (lastPx_ != 0) {
        const float moved = wrapDeg(yaw - lastYaw_);
        if (moved * static_cast<float>(lastPx_) > 0.f) {
            const float g = moved / static_cast<float>(lastPx_);
            if (g > 0.02f && g < 3.f)
                gainYaw_ = std::clamp(gainYaw_ * 0.75f + g * 0.25f, 0.03f, 2.f);
        }
        lastPx_ = 0;
    }
    if (lastPy_ != 0) {
        const float moved = pitch - lastPitch_;
        if (moved * static_cast<float>(lastPy_) > 0.f) {
            const float g = moved / static_cast<float>(lastPy_);
            if (g > 0.02f && g < 3.f)
                gainPitch_ = std::clamp(gainPitch_ * 0.75f + g * 0.25f, 0.03f, 2.f);
        }
        lastPy_ = 0;
    }
    const float dyaw = wrapDeg(wantYaw - yaw);
    const float dpitch = std::clamp(wantPitch - pitch, -89.f, 89.f);
    const bool within = std::abs(dyaw) <= tolerance && std::abs(dpitch) <= tolerance;
    if (std::abs(dyaw) <= 0.8f && std::abs(dpitch) <= 0.8f) {
        resX_ = resY_ = 0.f;
        return true;
    }
    const float stepYaw = std::clamp(dyaw * 0.45f, -7.f, 7.f);
    const float stepPitch = std::clamp(dpitch * 0.45f, -7.f, 7.f);
    const float wantPx = stepYaw / gainYaw_ + resX_;
    const float wantPy = stepPitch / gainPitch_ + resY_;
    const int px = static_cast<int>(std::lround(wantPx));
    const int py = static_cast<int>(std::lround(wantPy));
    resX_ = wantPx - static_cast<float>(px);
    resY_ = wantPy - static_cast<float>(py);
    if ((px || py) && feedMove(px, py)) {
        lastPx_ = px;
        lastPy_ = py;
        lastYaw_ = yaw;
        lastPitch_ = pitch;
    }
    return within;
}

bool AutoMineMod::faceAim(bedrock::Vec3 eye, bedrock::Vec3 target, float tolerance,
                          float smoothing) {
    if (!aimSmoothInit_) {
        aimSmooth_ = target;
        aimSmoothInit_ = true;
    }
    aimSmooth_.x += (target.x - aimSmooth_.x) * smoothing;
    aimSmooth_.y += (target.y - aimSmooth_.y) * smoothing;
    aimSmooth_.z += (target.z - aimSmooth_.z) * smoothing;
    return faceToward(eye, aimSmooth_, tolerance);
}

} // namespace automine::client
