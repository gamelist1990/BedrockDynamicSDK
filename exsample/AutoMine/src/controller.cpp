#include "auto_mine.hpp"
#include "diagnostics.hpp"

namespace automine::client {

void AutoMineMod::resetRun() {
    if (craftReady_)
        craft_.cancelJobs();
    maintenanceMove_ = false;
    maintenanceDrop_ = false;
    chainForUtility_ = false;
    chainForBuilding_ = false;
    workstationFailedCells_.clear();
    hotbarCacheTick_ = -1;
    toolSyncSlot_ = -1;
    digHandAllowed_ = false;
    toolSyncStage_ = toolSyncTicks_ = 0;
    toolSyncItem_.clear();
    parkourWaypoint_ = {INT_MIN, 0, 0};
    parkourPadWait_ = false;
    pillarLandingTicks_ = 0;
    parkourLandingTicks_ = 0;
    parkourLastTick_ = -1;
    releaseInputs();
    phase_ = Phase::Idle;
    targetId_.clear();
    blacklist_.clear();
    miningProgress_.clear();
    mineRejectedStances_.clear();
    mineRejectedFor_ = {INT_MIN, 0, 0};
    hazardZones_.clear();
    path_.clear();
    pathDig_.clear();
    pathBridge_.clear();
    pathParkour_.clear();
    pathPillar_.clear();
    pathIdx_ = 0;
    pathFor_ = {INT_MIN, 0, 0};
    replans_ = 0;
    scaffoldActive_ = false;
    scaffoldDig_ = false;
    scaffoldFor_ = {INT_MIN, 0, 0};
    placedPillars_.clear();
    collectDropId_.clear();
    collectEntityValid_ = false;
    collectTarget_ = {INT_MIN, 0, 0};
    collectDig_.reset();
    collectFailAttempts_ = 0;
    resetCollectionSweep();
    veinPlan_.clear();
    veinId_.clear();
    pickupTarget_ = {INT_MIN, 0, 0};
    swimSubmerged_ = 0;
    surfacing_ = false;
    wasInWater_ = false;
    combatEid_ = 0;
    bowDraw_ = 0;
    attackPulse_ = 0;
    boatTriedFor_ = {INT_MIN, 0, 0};
    chainActive_ = false;
    chainJobStarted_ = false;
    chainWaitingCobble_ = false;
    chainForFurnace_ = false;
    wantPickaxeChain_ = false;
    chainSteps_.clear();
    chainStep_ = 0;
    chainRetries_ = 0;
    chainNextRetryTick_ = 0;
    expStage_ = ExpStage::None;
    gatherActive_ = false;
    gatherDone_ = false;
    gatherFailed_ = false;
    scanIds_.clear();
    scanOverrideHeld_ = false;
    huntLegDone_ = false;
    huntBlocked_ = false;
    descendDone_ = false;
    descendFailed_ = false;
    descendCave_ = false;
    cobbleNavFails_ = 0;
    cobbleDescend_ = false;
    moveStage_ = MoveStage::None;
    moveFailed_ = false;
    smeltDone_ = false;
    smeltFailed_ = false;
    forageActive_ = false;
    forageEid_ = 0;
    navReturn_ = false;
    chainReturnTrips_ = 0;
    savedId_.clear();
    hasTarget_ = false;
    mined_ = 0;
    goalCount_ = 0;
    goalBase_ = 0;
    goalProgress_ = 0;
    goalItem_.clear();
    needTier_ = 0;
    aimSmoothInit_ = false;
    independentAim_ = false;
}

void AutoMineMod::startMining(std::string arg, int goal) {
    sehStopped_ = false;
    resetRun();
    if (independent_ && !independentReady_) {
        reply("このゲーム版の独立制御を読み込めなかったよ。AutoMine.logを確認してね");
        return;
    }
    targetId_ = localPart(arg);
    if (targetId_.empty() || isAir(targetId_)) {
        reply("ブロック名を指定してね（例: /mine iron_ore）");
        return;
    }
    phase_ = Phase::Scan;
    scanCursor_ = 0;
    bestDist2_ = 1e18;
    goalCount_ = goal;
    goalItem_ = dropOf(targetId_);
    goalBase_ = goal > 0 ? invCountOf(goalItem_) : 0;
    if (goal > 0)
        reply(targetId_ + " を掘って " + goalItem_ + " を " + std::to_string(goal) +
              " 個集めるよ（半径" + std::to_string(radius_) + "）");
    else
        reply(targetId_ + " を探して掘るよ（半径" + std::to_string(radius_) + "）");
    note("mine start: " + targetId_ +
         (goal > 0 ? " goal=" + std::to_string(goal) + "x" + goalItem_ +
                         " base=" + std::to_string(goalBase_)
                   : ""));
}

int AutoMineMod::invCountOf(const std::string &item) {
    // Exact id match: a substring count would call diamond_ore or a
    // diamond pickaxe "diamond" and fake goal/material counts.
    const auto have = craftReady_ ? chainCounts() : visibleItems();
    const auto it = have.find(item);
    return it == have.end() ? 0 : it->second;
}

bool AutoMineMod::goalReached() {
    if (goalCount_ <= 0)
        return false;
    return invCountOf(goalItem_) - goalBase_ >= goalCount_;
}

void AutoMineMod::finishGoal() {
    reply("目標達成！ " + goalItem_ + " を " + std::to_string(goalCount_) +
          " 個集めたよ（掘った数: " + std::to_string(mined_) + "）");
    note("goal reached: " + std::to_string(goalCount_) + "x" + goalItem_);
    releaseInputs();
    phase_ = Phase::Idle;
    hasTarget_ = false;
    goalCount_ = 0;
}

void AutoMineMod::stopMining(const std::string &why) {
    releaseInputs();
    if (phase_ != Phase::Idle)
        reply("止めたよ（掘った数: " + std::to_string(mined_) + "）" +
              (why.empty() ? "" : " " + why));
    phase_ = Phase::Idle;
    hasTarget_ = false;
    collectDropId_.clear();
    collectDig_.reset();
    collectFailAttempts_ = 0;
    resetCollectionSweep();
}

void AutoMineMod::handleLine(const std::string &line) {
    // line starts with "mine"; argument is the remainder.
    std::string rest = line.size() > 4 ? line.substr(4) : "";
    while (!rest.empty() && rest.front() == ' ')
        rest.erase(rest.begin());
    const auto sp = rest.find(' ');
    const auto arg = lower(rest.substr(0, sp));
    int goal = 0;
    if (sp != std::string::npos) {
        const auto num = rest.substr(sp + 1);
        if (!num.empty() && num.find_first_not_of("0123456789") == std::string::npos)
            goal = std::clamp(std::atoi(num.c_str()), 1, 999);
    }
    if (arg.empty() || arg == "status") {
        reply(statusText());
    } else if (arg == "path") {
        showPath_ = !showPath_;
        reply(showPath_ ? "経路表示を ON にしたよ" : "経路表示を OFF にしたよ");
    } else if (arg == "stop") {
        stopMining("");
    } else if (arg == "food") {
        startForage(goal > 0 ? goal : 3);
    } else if (arg == "mobs") {
        probePending_ = true; // actor reads must run on the tick thread
        reply("probe: queued");
    } else if (arg == "crashtest") {
        note("crashtest: deliberate fault queued (crash reporter self-test)");
        crashTestPending_ = true;
    } else if (enabled_) {
        startMining(arg, goal);
    } else {
        reply("設定で無効になっているよ");
    }
}

std::string AutoMineMod::statusText() const {
    switch (phase_) {
    case Phase::Idle:
        return "待機中。/mine <ブロック名> で開始";
    case Phase::Scan:
        return targetId_ + " を探索中…（掘った数: " + std::to_string(mined_) + "）" +
               (goalCount_ > 0 ? " 目標 " + goalItem_ + " " + std::to_string(goalProgress_) + "/" +
                                     std::to_string(goalCount_)
                               : "");
    case Phase::Navigate:
        return targetId_ + " へ移動中（掘った数: " + std::to_string(mined_) + "）";
    case Phase::Mine:
        return targetId_ + " を採掘中（掘った数: " + std::to_string(mined_) + "）";
    case Phase::Collect:
        return "ドロップ回収中（掘った数: " + std::to_string(mined_) + "）";
    case Phase::NeedTool:
        return "ツール待ち（掘った数: " + std::to_string(mined_) + "）";
    case Phase::Escape:
        return "水から浮上中…（掘った数: " + std::to_string(mined_) + "）";
    case Phase::Combat:
        return "敵と戦闘中！（掘った数: " + std::to_string(mined_) + "）";
    case Phase::Boat:
        return "ボートで移動中（掘った数: " + std::to_string(mined_) + "）";
    case Phase::Craft:
        return "クラフト中（掘った数: " + std::to_string(mined_) + "）";
    case Phase::Ascend:
        return "地上へ帰還中（掘った数: " + std::to_string(mined_) + "）";
    case Phase::Forage:
        return "食料調達中（狩った数: " + std::to_string(forageKills_) + "）";
    case Phase::Recover:
        return "死亡した場所へ荷物を回収に戻り中";
    }
    return "";
}

void AutoMineMod::dispatchLine(std::string line) {
    while (!line.empty() && (line.front() == '/' || line.front() == ' '))
        line.erase(line.begin());
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
        line.pop_back();
    if (line.empty())
        return;
    handleLine(line);
}

void AutoMineMod::tickGuarded() {
    if (sehStopped_)
        return;
    __try {
        tick();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        onSehFault("tick");
    }
}

void AutoMineMod::onSehFault(const char *where) {
    InterlockedIncrement(&crashrec::g_sehFaults);
    note(std::string("SEH fault caught in ") + where + " (section: " + crashrec::g_section +
         "); stopping the run for safety");
    try {
        releaseInputs();
    } catch (...) {
    }
    phase_ = Phase::Idle;
    sehStopped_ = true;
}

void AutoMineMod::craftUpdateInner(void *level) {
    try {
        if (pendingReset_)
            craft_.worldChanged();
        craft_.update(client_.load(), level, !anyScreenUp());
    } catch (...) {
        note("craft adapter update failed");
    }
}

// MSVC C2712: a function containing __try must not require object unwinding.
// The handler's side effects (note() takes std::string by value) therefore
// live in craftUpdateGuarded, and this helper holds only the __try call —
// the same split the SDK core uses for its game calls.
bool AutoMineMod::craftUpdateSeh(void *level) {
    __try {
        craftUpdateInner(level);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void AutoMineMod::craftUpdateGuarded(void *level) {
    if (craftUpdateSeh(level))
        return;
    InterlockedIncrement(&crashrec::g_sehFaults);
    note("craft adapter update faulted (SEH); disabling craft");
    craftReady_ = false;
}

void AutoMineMod::drawPathInner(bedrock::sdk::WorldRenderer &renderer) {
    try {
        drawPath(renderer);
    } catch (...) {
    }
}

void AutoMineMod::drawPathGuarded(bedrock::sdk::WorldRenderer &renderer) {
    __try {
        drawPathInner(renderer);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&crashrec::g_sehFaults);
        outlineReady_ = false;
    }
}

void AutoMineMod::markSection() {
    crashrec::g_ticks = static_cast<LONG>(tickCount_);
    switch (phase_) {
    case Phase::Idle:
        crashrec::setSection("tick/Idle");
        break;
    case Phase::Scan:
        crashrec::setSection("tick/Scan");
        break;
    case Phase::Navigate:
        crashrec::setSection("tick/Navigate");
        break;
    case Phase::Mine:
        crashrec::setSection("tick/Mine");
        break;
    case Phase::Collect:
        crashrec::setSection("tick/Collect");
        break;
    case Phase::NeedTool:
        crashrec::setSection("tick/NeedTool");
        break;
    case Phase::Escape:
        crashrec::setSection("tick/Escape");
        break;
    case Phase::Combat:
        crashrec::setSection("tick/Combat");
        break;
    case Phase::Boat:
        crashrec::setSection("tick/Boat");
        break;
    case Phase::Craft:
        crashrec::setSection("tick/Craft");
        break;
    case Phase::Descend:
        crashrec::setSection("tick/Descend");
        break;
    case Phase::Hunt:
        crashrec::setSection("tick/Hunt");
        break;
    case Phase::Smelt:
        crashrec::setSection("tick/Smelt");
        break;
    case Phase::Ascend:
        crashrec::setSection("tick/Ascend");
        break;
    case Phase::Forage:
        crashrec::setSection("tick/Forage");
        break;
    case Phase::Recover:
        crashrec::setSection("tick/Recover");
        break;
    }
}

void AutoMineMod::tick() {
    ++tickCount_;
    if ((tickCount_ % 200) == 0) // expire combat give-up entries
        for (auto it = combatIgnore_.begin(); it != combatIgnore_.end();)
            if (it->second <= tickCount_)
                it = combatIgnore_.erase(it);
            else
                ++it;
    if (pendingReset_) {
        controls_.clear();
        nativeMining_ = false;
        independentAim_ = false;
        pendingReset_ = false;
        gui_.reset(epoch_.load());
        camera_.reset();
        resetRun();
        joinGuardTicks_ = 20 * 60;
        joinGuardNoted_ = false;
        joinDryTicks_ = 0;
        joinSurfaced_ = false;
        if (recoverActive_)
            phase_ = Phase::Recover;
    }
    tickDeathWatch();
    if (recoverActive_ && !deathPending_ && phase_ != Phase::Recover && phase_ != Phase::Mine &&
        phase_ != Phase::Collect)
        phase_ = Phase::Recover; // the corpse run owns the client until done
    if (ascendActive_ && !recoverActive_ && !deathPending_ && phase_ != Phase::Ascend &&
        phase_ != Phase::Mine && phase_ != Phase::Collect)
        phase_ = Phase::Ascend; // the climb owns the client until it arrives or fails
    {
        std::deque<std::string> native;
        {
            std::lock_guard lock(nativeMutex_);
            native.swap(nativeLines_);
        }
        for (auto &line : native)
            dispatchLine(std::move(line));
    }
    if (probePending_) {
        probePending_ = false;
        bedrock::sdk::Vec3Value pf;
        if (feetPos(pf))
            probeActors(pf);
        else
            note("probe: no feet");
    }
    if (phase_ == Phase::Idle && (descendDone_ || huntLegDone_ || smeltDone_ || smeltFailed_)) {
        bedrock::sdk::Vec3Value pf;
        if (feetPos(pf)) {
            if (cobbleDescend_ && descendDone_) {
                cobbleDescend_ = false;
                descendDone_ = false;
                descendFailed_ = false;
                descendCave_ = false;
                phase_ = Phase::Scan;
                scanCursor_ = 0;
                bestDist2_ = 1e18;
                hasTarget_ = false;
                emptyScans_ = 0;
                note("craft chain: cobble shaft done, resuming stone scan");
            }
            if (huntLegDone_ && gatherActive_ && phase_ == Phase::Idle) {
                huntLegDone_ = false;
                if (huntBlocked_) {
                    huntBlocked_ = false;
                    const int ndx = huntDirZ_, ndz = -huntDirX_;
                    huntDirX_ = ndx;
                    huntDirZ_ = ndz;
                    note("gather: hunt direction rotated");
                }
                phase_ = Phase::Scan;
                scanCursor_ = 0;
                bestDist2_ = 1e18;
                hasTarget_ = false;
                emptyScans_ = 0;
            }
            if (phase_ == Phase::Idle) {
                if (forageActive_)
                    tickForage(pf);
                else
                    tickExpedition(pf);
            }
        }
    }
    if (phase_ == Phase::Idle) {
        bedrock::sdk::Vec3Value pf;
        if (feetPos(pf) && tickIdleGuards(pf))
            return;
        clearPath();
        return;
    }
    if (independent_ && anyScreenUp())
        controls_.clearManualInput();
    if (independent_ &&
        ((controls_.manualActive() && phase_ != Phase::Craft && phase_ != Phase::Smelt) ||
         !independentReady_ ||
         (automationScreenUp() && phase_ != Phase::Craft && phase_ != Phase::Smelt &&
           moveStage_ == MoveStage::None))) {
        miningProgress_.pause(tickCount_);
        releaseInputs();
        progressGoal_ = {INT_MIN, 0, 0};
        bedrock::sdk::Vec3Value pausedFeet;
        if (feetPos(pausedFeet))
            updatePath(pausedFeet);
        return;
    }
    if ((independent_ ? automationScreenUp() : anyScreenUp()) && phase_ != Phase::Craft &&
        phase_ != Phase::Smelt) {
        if (moveStage_ != MoveStage::None) {
            (void)ensureHotbarTick(moveItem_);
            return;
        }
        miningProgress_.pause(tickCount_);
        releaseInputs();
        // A user's screen is not ours to dismiss.
        return;
    }
    bedrock::sdk::Vec3Value feet;
    if (!feetPos(feet)) {
        miningProgress_.pause(tickCount_);
        releaseInputs();
        return;
    }
    if (!headInWater(feet)) {
        lastDryPos_ = feet;
        lastDryValid_ = true;
    }
    if (floodHold_) {
        if (headInWater(feet)) {
            floodDryTicks_ = 0;
            pressKey(kKeySpace, true);
            if (lastDryValid_ && ++floodWetTicks_ > 20 * 5) {
                bedrock::sdk::Vec3Value eye;
                if (eyePos(eye))
                    faceToward(eye, {lastDryPos_.x, eye.y, lastDryPos_.z}, 5.f);
                pressKey(kKeyW, true);
            }
        } else {
            pressKey(kKeySpace, false);
            pressKey(kKeyW, false);
            floodWetTicks_ = 0;
            if (++floodDryTicks_ > 20) {
                floodHold_ = false;
                floodDryTicks_ = 0;
                note("flood hold: breathing again, resuming");
            }
        }
        updatePath(feet);
        return;
    }
    if (phase_ == Phase::Escape) {
        tickEscape(feet);
        updatePath(feet);
        return;
    }
    if (phase_ == Phase::Mine || phase_ == Phase::Collect || phase_ == Phase::NeedTool ||
        phase_ == Phase::Combat || phase_ == Phase::Forage) {
        if (headInWater(feet)) {
            if (++submergedTicks_ >= 3) {
                startEscape();
                tickEscape(feet);
                updatePath(feet);
                return;
            }
        } else
            submergedTicks_ = 0;
    }
    // Hostile interruption: a mob that gets close during work preempts
    // the run — fight first, then resume (exitCombat restores the run).
    if (game_.gameplayActive() &&
        (phase_ == Phase::Scan || phase_ == Phase::Navigate || phase_ == Phase::Mine ||
         phase_ == Phase::Collect || phase_ == Phase::NeedTool || phase_ == Phase::Craft)) {
        if ((++threatScanTick_ % 4) == 0) {
            if (auto mob = scanHostile(feet, 12.0)) {
                const double dy = std::abs(mob->center.y - feet.y);
                if (dy <= 4.0 || mob->dist <= 6.0) {
                    enterCombat(*mob);
                    tickCombat(feet);
                    updatePath(feet);
                    return;
                }
            }
        }
    }
    if (phase_ == Phase::Collect) {
        // Recovery of the last drop owns the run, even if the sweep has to
        // wait. Only safety interruptions above and tool sync may preempt it.
        if (!tickToolSync())
            tickCollect(feet);
        updatePath(feet);
        return;
    }
    if (chainWaitingCobble_ && phase_ == Phase::Scan) {
        if (countAllItems("cobblestone") >= 3) {
            if (!chainTablePlaced_ && countAllItems("crafting_table") >= 1) {
                chainSteps_.insert(chainSteps_.begin() + chainStep_,
                                   ChainStep{ChainStep::Kind::PlaceTable});
                chainWaitingCobble_ = false;
                phase_ = Phase::Craft;
                chainStepTicks_ = 0;
                chainAimedTick_ = -1;
                note("craft chain: placing carried table for the stone pickaxe");
            } else if (!chainTablePlaced_) {
                chainFail("作業台が無い");
            } else if (workstationReachableFrom(feet, chainTablePos_)) {
                chainWaitingCobble_ = false;
                phase_ = Phase::Craft;
                chainStepTicks_ = 0;
                chainAimedTick_ = -1;
                note("craft chain: cobblestone ready, resuming");
            } else if (countAllItems("crafting_table") >= 1) {
                chainSteps_.insert(chainSteps_.begin() + chainStep_,
                                   ChainStep{ChainStep::Kind::PlaceTable});
                chainWaitingCobble_ = false;
                phase_ = Phase::Craft;
                chainStepTicks_ = 0;
                chainAimedTick_ = -1;
                note("craft chain: placing carried table for the stone pickaxe");
            } else if (countAllItems("planks") >= 4) {
                // No table at hand, but enough planks to make one on
                // the spot (personal grid) and place it right here.
                chainSteps_.insert(
                    chainSteps_.begin() + chainStep_,
                    {ChainStep{ChainStep::Kind::OpenInventory},
                     ChainStep{ChainStep::Kind::Craft, "minecraft:WorkBench_recipeId",
                               "minecraft:crafting_table", 1, false},
                     ChainStep{ChainStep::Kind::CloseUi}, ChainStep{ChainStep::Kind::PlaceTable}});
                chainWaitingCobble_ = false;
                phase_ = Phase::Craft;
                chainStepTicks_ = 0;
                chainAimedTick_ = -1;
                note("craft chain: crafting a fresh table on the spot");
            } else if (countAllItems("_log") >= 1 || countAllItems("_wood") >= 1) {
                int logCount = 0;
                const std::string wood = chainWood(chainCounts(), logCount);
                const int deficit = 4 - countAllItems("planks");
                if (!wood.empty() && deficit > 0) {
                    chainSteps_.insert(
                        chainSteps_.begin() + chainStep_,
                        {ChainStep{ChainStep::Kind::OpenInventory},
                         ChainStep{ChainStep::Kind::Craft, "minecraft:" + wood + "_planks",
                                   "minecraft:" + wood + "_planks", (deficit + 3) / 4, false},
                         ChainStep{ChainStep::Kind::Craft, "minecraft:WorkBench_recipeId",
                                   "minecraft:crafting_table", 1, false},
                         ChainStep{ChainStep::Kind::CloseUi},
                         ChainStep{ChainStep::Kind::PlaceTable}});
                    chainWaitingCobble_ = false;
                    phase_ = Phase::Craft;
                    chainStepTicks_ = 0;
                    chainAimedTick_ = -1;
                    note("craft chain: crafting planks and a table on the spot");
                } else if (++chainReturnTrips_ <= 3) {
                    target_ = chainTablePos_;
                    hasTarget_ = true;
                    navReturn_ = true;
                    invalidateRoute();
                    navTicks_ = stuckTicks_ = replans_ = jumpHoldTicks_ = 0;
                    phase_ = Phase::Navigate;
                    note("craft chain: returning to the crafting table");
                } else {
                    chainWaitingCobble_ = false;
                    chainActive_ = false;
                    reply("作業台に戻れないから、石のツルハシは諦めるね");
                    note("craft chain: could not return to the table");
                }
            } else if (++chainReturnTrips_ <= 3) {
                target_ = chainTablePos_;
                hasTarget_ = true;
                navReturn_ = true;
                invalidateRoute();
                navTicks_ = stuckTicks_ = replans_ = jumpHoldTicks_ = 0;
                phase_ = Phase::Navigate;
                note("craft chain: returning to the crafting table");
            } else {
                chainWaitingCobble_ = false;
                chainActive_ = false;
                reply("作業台に戻れないから、石のツルハシは諦めるね");
                note("craft chain: could not return to the table");
            }
        } else if (cobbleNavFails_ >= 2) {
            cobbleNavFails_ = 0;
            descendTargetY_ = static_cast<int>(std::floor(feet.y)) - 10;
            descendDone_ = false;
            descendFailed_ = false;
            descendCave_ = false;
            descendLastY_ = INT_MAX;
            descendStuck_ = 0;
            descendWide_ = false;
            descendCenterTicks_ = 0;
            descendEntryY_ = static_cast<int>(std::floor(feet.y));
            cobbleDescend_ = true;
            releaseInputs();
            phase_ = Phase::Descend;
            note("craft chain: cobble targets unreachable, digging down to stone");
        } else if (++chainCobbleTicks_ > 20 * 180) {
            chainWaitingCobble_ = false;
            chainActive_ = false;
            reply("丸石が集まらないから、石のツルハシは諦めるね");
            note("craft chain: cobblestone wait timed out");
        }
    }
    if (goalCount_ > 0 && phase_ != Phase::Idle && collectDropId_.empty() &&
        tickCount_ % 20 == 0) {
        goalProgress_ = goalItem_ == targetId_ ? mined_ : invCountOf(goalItem_) - goalBase_;
        if (goalReached()) {
            finishGoal();
            return;
        }
    }
    if (scanOverrideHeld_ && !chainWaitingCobble_ && !gatherActive_)
        endScanOverride();
    // Expedition gather completion / timeout.
    if (gatherActive_ && collectDropId_.empty()) {
        int total = 0;
        for (const auto &id : gatherCountIds_)
            total += countAllItems(id);
        if (total >= gatherWant_) {
            gatherActive_ = false;
            gatherDone_ = true;
            releaseInputs();
            phase_ = Phase::Idle;
            hasTarget_ = false;
            note("gather: done (" + std::to_string(total) + " collected)");
        } else if (tickCount_ > gatherDeadline_) {
            gatherActive_ = false;
            gatherFailed_ = true;
            releaseInputs();
            phase_ = Phase::Idle;
            hasTarget_ = false;
            note("gather: timed out");
        } else if (gatherHunt_ && phase_ == Phase::Scan &&
                   (emptyScans_ >= 3 || escapeStreak_ >= 3) && !chainActive_) {
            if (escapeStreak_ >= 3)
                note("gather: escape loop (x" + std::to_string(escapeStreak_) + "), relocating");
            escapeStreak_ = 0;
            emptyScans_ = 0;
            huntStepActive_ = false;
            huntStage_ = 0;
            huntCells_ = 0;
            huntLegDone_ = false;
            digPos_ = {INT_MIN, 0, 0};
            phase_ = Phase::Hunt;
            note("gather: hunt leg (" + std::to_string(huntDirX_) + "," +
                 std::to_string(huntDirZ_) + ")");
        }
    }
    if (phase_ == Phase::Scan && emptyScans_ >= 6 && !chainActive_ && !gatherActive_) {
        if (!expeditionActive())
            (void)maybeStartExpedition();
        if (!expeditionActive() && goalCount_ > 0 && requiredTier(targetId_) >= 4 &&
            hasPickaxeTier(requiredTier(targetId_))) {
            needTier_ = requiredTier(targetId_);
            startExpedition(feet.y > -40 ? ExpStage::ToDiamondLevel : ExpStage::HuntOre);
        }
    }
    if (huntLegDone_ && gatherActive_ && phase_ == Phase::Idle) {
        huntLegDone_ = false;
        if (huntBlocked_) {
            // Rotate 90 degrees: the old heading is a cave edge, a
            // lava pocket or a wall the advance could not pass.
            huntBlocked_ = false;
            const int ndx = huntDirZ_, ndz = -huntDirX_;
            huntDirX_ = ndx;
            huntDirZ_ = ndz;
            note("gather: hunt direction rotated");
        }
        phase_ = Phase::Scan;
        scanCursor_ = 0;
        bestDist2_ = 1e18;
        hasTarget_ = false;
        emptyScans_ = 0;
    }
    if (cobbleDescend_ && descendDone_) {
        cobbleDescend_ = false;
        descendDone_ = false;
        descendFailed_ = false;
        descendCave_ = false;
        phase_ = Phase::Scan;
        scanCursor_ = 0;
        bestDist2_ = 1e18;
        hasTarget_ = false;
        emptyScans_ = 0;
        note("craft chain: cobble shaft done, resuming stone scan");
    }
    if (tickToolSync()) {
        updatePath(feet);
        return;
    }
    // Don't schedule optional expedition/tool work over a live destroy session.
    if (collectDropId_.empty() && !(phase_ == Phase::Mine && independent_ && attackHeld_))
        tickExpedition(feet);
    if (collectDropId_.empty() && tickInventoryMaintenance(feet)) {
        updatePath(feet);
        return;
    }
    if (forageActive_ && (phase_ == Phase::Forage || phase_ == Phase::Scan))
        tickForage(feet);
    if (hasTarget_ && (phase_ == Phase::Navigate || phase_ == Phase::Mine) && !recoverActive_ &&
        !ascendActive_ && !scaffoldDig_) {
        const auto goal = savedId_.empty() ? target_ : savedTarget_;
        const double d =
            std::hypot(goal.x + .5 - feet.x, goal.y + .5 - feet.y - 1.62, goal.z + .5 - feet.z);
        if (!(progressGoal_ == goal)) {
            progressGoal_ = goal;
            progressBest_ = d;
            progressMined_ = mined_;
            progressTick_ = tickCount_;
        } else if (d < progressBest_ - .5 || mined_ != progressMined_) {
            progressBest_ = std::min(progressBest_, d);
            progressMined_ = mined_;
            progressTick_ = tickCount_;
        } else if (tickCount_ - progressTick_ > 20 * 90) {
            note("route: no objective progress for 90s; choosing another target");
            blacklistCluster(goal, savedId_.empty() ? targetId_ : savedId_);
            restoreMineThroughGoal();
            hasTarget_ = false;
            releaseInputs();
            path_.clear();
            phase_ = Phase::Scan;
            scanCursor_ = 0;
            bestDist2_ = 1e18;
        }
    } else
        progressGoal_ = {INT_MIN, 0, 0};
    markSection();
    if (phase_ != Phase::Mine && phase_ != Phase::Navigate)
        miningProgress_.pause(tickCount_);
    if (phase_ != Phase::Navigate && phase_ != Phase::Forage)
        pressKey(kKeySprint, false);
    switch (phase_) {
    case Phase::Scan:
        tickScan(feet);
        break;
    case Phase::Navigate:
        tickNavigate(feet);
        break;
    case Phase::Mine:
        tickMine(feet);
        break;
    case Phase::Collect:
        break; // prioritized before scheduling above
    case Phase::NeedTool:
        tickNeedTool(feet);
        break;
    case Phase::Ascend:
        tickAscend(feet);
        break;
    case Phase::Combat:
        tickCombat(feet);
        break;
    case Phase::Boat:
        tickBoat(feet);
        break;
    case Phase::Craft:
        tickCraft(feet);
        break;
    case Phase::Descend:
        tickDescend(feet);
        break;
    case Phase::Hunt:
        tickHunt(feet);
        break;
    case Phase::Smelt:
        tickSmelt(feet);
        break;
    case Phase::Forage:
        break; // driven beside tickExpedition above
    case Phase::Recover:
        tickRecover(feet);
        break;
    case Phase::Idle:
    case Phase::Escape:
        break;
    }
    // Refresh remaining actions on EVERY walking tick, not just planPath().
    // Recover/Ascend share the normal route cache consumed by onRenderLevel.
    updatePath(feet);
}

} // namespace automine::client
