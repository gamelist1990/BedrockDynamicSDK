#pragma once

#include "action_plan.hpp"
#include "navigation_geometry.hpp"
#include "locomotion_geometry.hpp"
#include "mining_progress.hpp"
#include "inventory_policy.hpp"
#include <bedrock/sdk.hpp>
#include <nlohmann/json.hpp>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <chrono>
#include <climits>
#include <cmath>
#include <functional>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if !defined(_MSC_VER) || _ITERATOR_DEBUG_LEVEL != 0
#error AutoMine requires MSVC Release STL ABI.
#endif

#include "block_knowledge.hpp"

namespace automine::client {

using namespace std::chrono_literals;

inline constexpr unsigned kKeyW = 0x57, kKeyA = 0x41, kKeyS = 0x53, kKeyD = 0x44;
inline constexpr unsigned kKeySpace = 0x20, kKeySprint = 0xA2, kKeySneak = 0x10;

class AutoMineMod {
  public:
    std::atomic<unsigned> status{};
    void signalStop();
    void stop();
    void run();

  private:
    using GetTopScreenName = void *(__fastcall *)(void *, void *);

    struct InvSlot {
        std::string id;
        int count{};
        bool available{};
        int damage{-1};
        std::int32_t netId{};
    };

    struct ToolChoice {
        int slot{-1};
        int tier{};
        ToolClass kind{ToolClass::None};
        bool byHand{};
    };

    struct MobInfo {
        std::uint32_t eid{};
        std::uint32_t type{};
        bedrock::sdk::Vec3Value center{};
        bedrock::sdk::Vec3Value bmin{};
        bedrock::sdk::Vec3Value bmax{};
        bool hasBounds{};
        double dist{1e18};
    };

    struct Weapon {
        int slot{-1};
        int kind{0};
        int tier{-1};
    };

    enum class CombatMode { Melee, Bow, Flee };

    enum class BoatStep { Deploy, Board, Ride, Dismount };

    enum class Phase {
        Idle,
        Scan,
        Navigate,
        Mine,
        Collect,
        NeedTool,
        Escape,
        Combat,
        Boat,
        Craft,
        Descend,
        Hunt,
        Smelt,
        Ascend,
        Forage,
        Recover
    };

    enum class ForageStage { Hunt, Cook, Eat };

    enum class CellKind : std::uint8_t { Stand, Dig, Blocked, Bridge };

    struct CellKey {
        int x{}, y{}, z{};
        bool operator==(const CellKey &) const noexcept = default;
    };

    struct CellKeyHash {
        std::size_t operator()(const CellKey &k) const noexcept {
            return std::hash<long long>{}((static_cast<long long>(k.x) * 73856093LL) ^
                                          (static_cast<long long>(k.y) * 19349663LL) ^
                                          (static_cast<long long>(k.z) * 83492791LL));
        }
    };

    struct CellInfo {
        CellKind kind{CellKind::Blocked};
        double digCost{0.0};
    };

    struct ChainStep {
        enum class Kind { Craft, PlaceTable, OpenTable, CloseUi, WaitCobble, OpenInventory };
        Kind kind{Kind::Craft};
        std::string recipeId, outputName;
        int times{1};
        bool tableGrid{};
    };

    enum class MoveStage { None, OpenInv, Evict, Bring, CloseUi };

    enum class SmeltStage {
        CraftFurnace,
        PlaceFurnace,
        OpenFurnace,
        LoadInput,
        LoadFuel,
        CloseUi,
        WaitSmelt,
        BreakFurnace,
        CollectDrops,
        Done,
        Failed
    };

    enum class ExpStage {
        None,
        Wood,
        StonePick,
        ToIronLevel,
        HuntIron,
        Cobble,
        Smelt,
        IronPick,
        ToDiamondLevel,
        HuntOre
    };

    struct HazardZone {
        int x, y, z, r;
    };

    using CellCache = std::unordered_map<CellKey, CellInfo, CellKeyHash>;

    // Lifecycle and configuration (runtime.cpp).
    void note(std::string text);
    void flush(std::ostream &out);
    void reply(const std::string &text);
    static std::filesystem::path directory();
    void loadConfig(const std::filesystem::path &dir);
    void initialize();
    bool loadWorld(const bedrock::sdk::Profile &profile, std::string &failure);

    // Commands and tick orchestration (controller.cpp).
    void resetRun();
    void startMining(std::string arg, int goal = 0);
    int invCountOf(const std::string &item);
    bool goalReached();
    void finishGoal();
    void stopMining(const std::string &why);
    void handleLine(const std::string &line);
    std::string statusText() const;
    void dispatchLine(std::string line);
    void tickGuarded();
    void onSehFault(const char *where);
    void craftUpdateInner(void *level);
    bool craftUpdateSeh(void *level);
    void craftUpdateGuarded(void *level);
    void drawPathInner(bedrock::sdk::WorldRenderer &renderer);
    void drawPathGuarded(bedrock::sdk::WorldRenderer &renderer);
    void markSection();
    void tick();

    // Movement input and world queries (controls.cpp).
    bool releaseNativeKey(unsigned key);
    bool setNativeMouseDown(unsigned button, bool down);
    bool feedMove(int dx, int dy);
    bool feedButton(bool down);
    bool feedUseButton(bool down);
    void setUse(bool down);
    void pressKey(unsigned key, bool down);
    void setAttack(bool down);
    void releaseInputs();
    bool independentHit(bedrock::sdk::Vec3Value eye, BlockPos3 &cell, unsigned &face, bedrock::sdk::Vec3Value &hit);
    void flushIndependent();
    void *player();
    void *region();
    bool feetPos(bedrock::sdk::Vec3Value &out);
    bool eyePos(bedrock::sdk::Vec3Value &out);
    std::string blockAt(BlockPos3 pos);
    bool lineOfSight(bedrock::sdk::Vec3Value eye, BlockPos3 targetCell, BlockPos3 &hit);
    bool rotation(float &yaw, float &pitch);
    static float wrapDeg(float deg);
    bool faceToward(bedrock::sdk::Vec3Value eye, bedrock::sdk::Vec3Value target, float tolerance = 3.f);
    bool faceAim(bedrock::sdk::Vec3Value eye, bedrock::sdk::Vec3Value target, float tolerance, float smoothing);

    // Screen state (screens.cpp).
    void loadScreen(const bedrock::sdk::Profile &profile,
                    const bedrock::sdk::SymbolResolver &resolver);
    static bool callTopScreenNameSeh(GetTopScreenName fn, void *client, void *buf);
    std::string topScreenName();
    bool inventoryScreenUp();
    bool screenUp(const char *needle);
    bool anyScreenUp();
    bool automationScreenUp();

    // Inventory transfers and maintenance (inventory.cpp).
    std::array<InvSlot, 9> hotbarMemoryItems();
    std::array<InvSlot, 9> hotbarItems();
    std::vector<int> hotbarSlotCandidates(const std::string &local);
    int placeSlotTick(const std::string &local, std::size_t &idx, int &ticks, int fallback);
    void selectSlot(int slot);
    bool isSpareBlock(const std::string &local);
    static int buildingReserve(const std::string &id);
    std::array<InvSlot, 9> buildingHotbar();
    int spareCount();
    int totalSpareCount();
    int spareSlot();
    bool prepareBuildingSupply();
    std::set<BlockPos3> workstationFailedCells_;
    bool chainForBuilding_{};
    long long buildingCraftNext_{};
    int scaffoldSlot();
    int scaffoldCount();
    int countAllItems(std::string_view key);
    std::unordered_map<std::string, int> chainCounts();
    static int countIn(const std::unordered_map<std::string, int> &items, std::string_view key);
    static std::uint32_t invContainer(int);
    static int preferredToolSlot(const std::string &id);
    bool ensureHotbarTick(const std::string &rawId, int sourceSlot = -1);
    bool tickInventoryMaintenance(bedrock::sdk::Vec3Value feet);
    int findHotbarSlot(const std::string &localId);
    std::optional<bedrock::sdk::CraftInventorySlotValue> findInvItem(const std::string &localId);
    std::optional<bedrock::sdk::CraftInventorySlotValue>
    findInvItemPred(const std::function<bool(const std::string &)> &pred);

    // Tool selection and durability (tools.cpp).
    static int maxDurabilityOf(const std::string &local);
    void chargeToolUse();
    int toolRemainingAt(int slot);
    int bestPickaxeRemaining(int minTier);
    bool hasSparePickaxe(int minTier);
    ToolChoice chooseTool(const std::string &blockId);
    bool ensureTool(const std::string &blockId);
    bool tickToolSync();
    std::unordered_map<std::string, int> visibleItems();
    void enterNeedTool();
    void tickNeedTool(bedrock::sdk::Vec3Value feet);

    // Actors and combat (combat.cpp).
    static bool isHostileType(std::uint32_t t);
    static bool isCreeperType(std::uint32_t t);
    std::optional<MobInfo> scanHostile(const bedrock::sdk::Vec3Value &feet, double maxDist,
                                       std::uint32_t onlyEid = 0);
    static bool isFoodAnimalType(std::uint32_t t);
    static const char *rawMeatOf(std::uint32_t t);
    static std::string cookedMeatOf(const std::string &raw);
    std::optional<MobInfo> scanPassive(const bedrock::sdk::Vec3Value &feet, double maxDist,
                                       std::uint32_t onlyEid = 0);
    static double distToMob(const bedrock::sdk::Vec3Value &p, const MobInfo &m);
    static int weaponTierOf(const std::string &lp);
    Weapon bestMelee();
    int bowSlot();
    bool hasArrows();
    void enterCombat(const MobInfo &mob);
    void exitCombat(bool won);
    void tickCombat(const bedrock::sdk::Vec3Value &feet);
    bool loadActors(const bedrock::sdk::Profile &profile,
                    const bedrock::sdk::SymbolResolver &resolver, std::string &failure);
    std::optional<std::uint32_t> actorTypeOf(void *actor);
    // One registry capture of the current level's actors (facade samples;
    // sample.actorKey is the native actor pointer), or nullopt on failure.
    std::optional<std::vector<bedrock::sdk::ActorSample>> actorSamples();
    void probeActors(const bedrock::sdk::Vec3Value &feet);

    // Boat travel (boat.cpp).
    int boatSlot();
    std::optional<MobInfo> findBoat(const bedrock::sdk::Vec3Value &feet, double maxDist,
                                    std::uint32_t onlyEid = 0, bool freshOnly = false);
    void enterBoat();
    void giveUpBoat();
    void tickBoat(const bedrock::sdk::Vec3Value &feet);

    // Recovery and water safety (recovery.cpp).
    void tickDeathWatch();
    void startRecovery();
    void recoverDone(bool arrived);
    void tickRecover(const bedrock::sdk::Vec3Value &feet);
    bool tickJoinGuard(const bedrock::sdk::Vec3Value &feet);
    bool tickIdleGuards(const bedrock::sdk::Vec3Value &feet);
    bool headInWater(const bedrock::sdk::Vec3Value &feet);
    bool inWaterNow(const bedrock::sdk::Vec3Value &feet);
    void startEscape();
    void tickEscape(const bedrock::sdk::Vec3Value &feet);

    // Action preview and rendering (path_render.cpp).
    void clearPath();
    automine::Terrain planTerrain(automine::PlanCell p);
    automine::ActionPlan futureActions(const bedrock::sdk::Vec3Value &feet);
    automine::ActionPlan plannedActions(const bedrock::sdk::Vec3Value &feet);
    std::optional<BlockPos3> previewWorkCell();
    void updatePath(const bedrock::sdk::Vec3Value &feet);
    void drawPath(bedrock::sdk::WorldRenderer &renderer);

    // Target selection (target_selection.cpp).
    void planVein();
    void finishCollection(bedrock::sdk::Vec3Value feet);
    void startCollection();
    void resetCollectionSweep();
    void tickScan(bedrock::sdk::Vec3Value feet);

    // Terrain classification and route search (pathfinding.cpp).
    bool isPassableCell(const std::string &id);
    bool isFluidCell(const std::string &id);
    bool isSolidCell(const std::string &id);
    bool isDiggableCell(const std::string &id);
    static double blockDigCost(const std::string &id);
    CellKind classifyCell(CellKey c, CellCache &cache);
    double digCostOf(CellKey c, CellCache &cache);
    bool isGravityBlock(const std::string &local);
    void discardPathIfExhausted();
    void invalidateRoute();
    bool planPath(bedrock::sdk::Vec3Value feet);
    bool workstationReachableFrom(bedrock::sdk::Vec3Value feet, BlockPos3 table);

    // Route execution (navigation.cpp).
    void tickFollowPath(bedrock::sdk::Vec3Value feet);
    void tickNavigate(bedrock::sdk::Vec3Value feet);

    // Bridging, parkour and scaffolding (construction.cpp).
    void enterMineCell(BlockPos3 cell, const std::string &id);
    void enterPathDigCell(BlockPos3 cell, const std::string &id);
    bool tickBridge(bedrock::sdk::Vec3Value feet);
    void tickPillarWaypoint(bedrock::sdk::Vec3Value feet);
    bool wallLandingAnchor(BlockPos3 floor);
    bool placeLandingPad(BlockPos3 floor, bedrock::sdk::Vec3Value eye, std::string *placedId = nullptr);
    bool tickParkourWaypoint(bedrock::sdk::Vec3Value feet);
    bool isScaffoldBlock(const std::string &local);
    bool scaffoldWanted(bedrock::sdk::Vec3Value feet);
    bool scaffoldStanceOk(bedrock::sdk::Vec3Value feet);
    void engageScaffold(bedrock::sdk::Vec3Value feet);
    void endScaffold();
    void scaffoldFail(const std::string &why);
    bool tickScaffold(bedrock::sdk::Vec3Value feet);

    // Mining and collection (mining.cpp).
    bool tryMineThrough(bedrock::sdk::Vec3Value feet);
    void tickMine(bedrock::sdk::Vec3Value feet);
    void blacklistCluster(const BlockPos3 &center, const std::string &id);
    void tickCollect(bedrock::sdk::Vec3Value feet);
    void restoreMineThroughGoal();
    void endScanOverride();
    void startGather(std::vector<std::string> blockIds, std::vector<std::string> countIds, int want,
                     bool hunt);
    bool lavaAdjacent(const BlockPos3 &cell);
    int digCell(const BlockPos3 &cell, bedrock::sdk::Vec3Value feet, bool toolOptional = false);

    // Crafting chains (crafting.cpp).
    bool routeCrossesWater(const bedrock::sdk::Vec3Value &feet, const bedrock::sdk::Vec3Value &center);
    bool placedTableExists();
    int findTableHotbarSlot();
    static std::string chainWood(const std::unordered_map<std::string, int> &items, int &logCount);
    bool tryStartUtilityChain();
    bool tryStartPickaxeChain(bool force = false);
    bool tryStartSwordChain();
    void ensureSwordPrep();
    bool tryStartBoatChain();
    void chainAdvance();
    void chainFinish();
    void chainFail(const std::string &why);
    void dispatchCraftStep(const ChainStep &step);
    void tickCraft(bedrock::sdk::Vec3Value feet);

    // Table placement and interaction (crafting_table.cpp).
    int digPocketTick(bedrock::sdk::Vec3Value feet, BlockPos3 &pocket, int &badMask, int &stage);
    void tickPlaceTable(bedrock::sdk::Vec3Value feet);
    bool placeWorkstation(BlockPos3 cell, bedrock::sdk::Vec3Value eye);
    bool workstationCellReady(BlockPos3 cell, bedrock::sdk::Vec3Value eye);
    bool tableBanned(const BlockPos3 &p);
    void tickOpenTable(bedrock::sdk::Vec3Value feet);
    void tickOpenInventory();
    void tickCloseUi();

    // Ascending and descending (excavation.cpp).
    void enterAscend(int surfY, const std::string &replyText, const std::string &noteText);
    void rotateAscendDir();
    void tickAscend(bedrock::sdk::Vec3Value feet);
    void tickDescend(bedrock::sdk::Vec3Value feet);
    void tickHunt(bedrock::sdk::Vec3Value feet);

    // Smelting (smelting.cpp).
    static std::string smeltYieldOf(const std::string &item);
    void startSmelt(const std::string &item, int count);
    void smeltAdvance();
    void smeltFail(const std::string &why);
    bool tryStartFurnaceChain();
    bool tickSmeltTransfer(std::uint32_t dstContainer, int dstSlot, bool isFuel);
    void tickSmelt(bedrock::sdk::Vec3Value feet);
    void tickPlaceFurnace(bedrock::sdk::Vec3Value feet);
    void tickOpenFurnace(bedrock::sdk::Vec3Value feet);

    // Food gathering (forage.cpp).
    void startForage(int want);
    void forageFail(const std::string &why);
    void forageDone();
    int forageRawTotal();
    void tickForage(const bedrock::sdk::Vec3Value &feet);

    // Expedition supervision (expedition.cpp).
    bool expeditionActive() const;
    bool maybeStartExpedition();
    int heldPickaxeTier();
    bool hasPickaxeTier(int minTier);
    void startExpedition(ExpStage stage);
    void expFail(const std::string &why);
    void enterExpStage(ExpStage stage);
    void beginDescend(ExpStage stage);
    bool descendCaveResume(ExpStage stage, bedrock::sdk::Vec3Value feet);
    bool descendResumeTick(ExpStage stage);
    void tickExpedition(bedrock::sdk::Vec3Value feet);

    // Shared state.

    std::array<InvSlot, 9> hotbarCache_;
    long long hotbarCacheTick_{-1};
    int toolSyncSlot_{-1}, toolSyncStage_{}, toolSyncTicks_{};
    int hotbarMoveAttempts_{};
    BlockPos3 hotbarMoveTarget_{INT_MIN, 0, 0};
    std::string hotbarMoveUnavailableItem_;
    bool digHandAllowed_{};
    std::string toolSyncItem_;

    std::array<std::string, 9> toolIdAtSlot_;
    std::array<int, 9> toolUsedAtSlot_{};
    std::array<std::int32_t, 9> toolNetAtSlot_{};

    GetTopScreenName topScreenFn_{};
    void *memDeallocate_{};
    bool screenReady_{};

    std::vector<ChainStep> chainSteps_;
    std::size_t chainStep_{};
    bool chainActive_{};
    bool chainJobStarted_{};
    int chainStepTicks_{};
    int chainAimedTick_{-1};
    bool chainRetriedOpen_{};
    int chainRecoverTicks_{-1};
    std::string chainPendingFail_;
    Phase chainReturn_{Phase::Scan};
    bool chainForPickaxe_{};
    bool chainForFurnace_{};
    bool wantPickaxeChain_{};
    int needTier_{};
    long long chainNextRetryTick_{};
    int chainRetries_{};
    bool chainWaitingCobble_{};
    bool navReturn_{};
    int chainReturnTrips_{};
    int chainSubmerged_{};
    int chainCobbleTicks_{};
    BlockPos3 chainTablePos_{INT_MIN, 0, 0};
    bool chainTablePlaced_{};
    BlockPos3 chainTableBad_{INT_MIN, 0, 0};
    long long chainTableBadUntil_{};
    BlockPos3 chainPlaceCell_{INT_MIN, 0, 0};
    BlockPos3 chainPocket_{INT_MIN, 0, 0};
    int chainPocketBad_{};
    int chainPocketStage_{};
    bool chainPocketing_{};
    bool chainPocketDug_{};
    BlockPos3 chainPocketCell_{INT_MIN, 0, 0};
    int chainNoSlotTicks_{};
    bool chainPlaceArmed_{};
    std::size_t chainPlaceSlotIdx_{};
    int chainPlaceSlotTicks_{};
    int chainNoSlotRetries_{};
    std::vector<BlockPos3> chainPlaceCells_;
    std::size_t chainPlaceIdx_{};
    int placeCellTicks_{};
    BlockPos3 boatChainTriedFor_{INT_MIN, 0, 0};

    MoveStage moveStage_{MoveStage::None};
    std::string moveItem_;
    int moveSrc_{-1}, moveDst_{-1}, moveCount_{};
    std::int32_t moveSrcNet_{};
    int moveEvict_{-1}, moveEvictCount_{}, moveFreeMain_{-1};
    std::int32_t moveEvictNet_{};
    std::string moveEvictId_;
    bool moveNeedEvict_{};
    int moveAttempts_{};
    int moveScreenOkTicks_{-1};
    bool moveReplan_{};
    bool moveEvictDone_{};
    bool moveFailPending_{};
    long long moveStageStart_{};
    long long moveDeadline_{};
    bool moveFailed_{};
    int moveRetryTick_{};

    bedrock::sdk::ItemStackRequests craft_;
    bool craftReady_{};
    std::optional<bedrock::sdk::Profile> craftProfile_;
    bedrock::sdk::RosterAccess access_;
    bedrock::sdk::Hooks game_;
    bedrock::sdk::WorldAccess world_;
    bedrock::sdk::ActorFacade actors_;
    bool actorsReady_{false};
    bedrock::sdk::CameraControl camera_;
    bedrock::sdk::AutomationControls controls_;
    bedrock::sdk::BlockActions blockActions_;
    bool independent_{true}, independentReady_{}, independentAim_{};
    BlockPos3 parkourWaypoint_{INT_MIN, 0, 0}, parkourFrom_{};
    unsigned long long pathRevision_{}, parkourRevision_{}, parkourLaunchTick_{};
    int parkourStage_{}, parkourTicks_{};
    bool parkourRose_{};
    bedrock::sdk::Vec3Value parkourLastFeet_{};
    long long parkourLastTick_{-1};
    int parkourLandingTicks_{};
    bool parkourPadWait_{};
    long long parkourPadTick_{};
    bool parkourSprintInput_{};
    unsigned long long parkourSprintStart_{};
    long long parkourRunTick_{};
    int parkourInputLog_{};
    bool continueInChat_{true};
    bool backgroundCraft_{true};
    bool autoDiscard_{true};
    std::set<std::string> discardItems_;
    bool maintenanceMove_{}, maintenanceDrop_{}, chainForUtility_{};
    std::string maintenanceItem_, utilityOutput_;
    unsigned long long maintenanceNext_{}, utilityNext_{};
    float independentYaw_{}, independentPitch_{};
    bool nativeMining_{}, nativeUsing_{}, usePulsePending_{}, nativeActionWarned_{};
    int nativeMiningMissTicks_{};
    automine::MiningProgress<BlockPos3> miningProgress_;
    unsigned long long mineAimTick_{};
    BlockPos3 mineAttemptStance_{};
    BlockPos3 mineRejectedFor_{INT_MIN, 0, 0};
    std::set<BlockPos3> mineRejectedStances_;
    float destroyProgress_{};
    bedrock::sdk::BlockPos destroyTarget_{-1, -1, -1};
    std::uint64_t lastNativeActionWarn_{};
    BlockPos3 nativeMiningCell_{};
    BlockPos3 mineOccluder_{INT_MIN, 0, 0};
    int mineOccluderStreak_{};
    int lastNativeUse_{-1000}, lastNativeAttack_{-1000};
    std::optional<bedrock::sdk::CameraPlayerSnapshot> snap_;
    bedrock::sdk::SafeTeleport gui_;
    bedrock::sdk::Commands injected_;
    std::vector<bedrock::sdk::CommandEnum> commandEnums_;
    std::vector<bedrock::sdk::CommandDefinition> commandDefs_;
    bedrock::sdk::SymbolResolver resolver_;
    std::mutex nativeMutex_;
    std::deque<std::string> nativeLines_;
    std::atomic<void *> client_{};
    std::atomic<void *> level_{};
    std::atomic<std::uint64_t> epoch_{1};
    std::atomic<bool> stopped_{};
    bool sehStopped_{};
    std::atomic<bool> crashTestPending_{};
    bool enabled_{true};
    int radius_{24};

    std::mutex logMutex_;
    std::deque<std::string> logs_;
    std::set<unsigned> heldKeys_;
    bool attackHeld_{};
    bool useHeld_{};

    Phase resumePhase_{Phase::Scan};
    CombatMode combatMode_{CombatMode::Melee};
    std::uint32_t combatEid_{};
    std::uint32_t combatType_{};
    int combatTicks_{};
    int attackCd_{};
    int attackPulse_{};
    int retreatTicks_{};
    bool retreatAfterHit_{};
    int bowDraw_{};
    int bowCd_{};
    int landedHits_{};
    int threatScanTick_{};
    int lastEngageTick_{};
    long long tickCount_{};
    std::unordered_map<std::uint32_t, long long> combatIgnore_;

    BoatStep boatStep_{BoatStep::Deploy};
    int boatTicks_{};
    int boatStepTicks_{};
    int boatPulse_{};
    int boatCd_{};
    int boatLockTicks_{};
    std::uint32_t boatEid_{};
    std::vector<std::uint32_t> preBoatEids_;
    int boatGlueTicks_{};
    int boatProbeTicks_{};
    bedrock::sdk::Vec3Value boatProbeStart_{};
    bedrock::sdk::Vec3Value boatCenter_{};
    BlockPos3 boatTriedFor_{INT_MIN, 0, 0};
    bool pendingReset_{};

    float gainYaw_{0.15f}, gainPitch_{0.15f};
    float resX_{}, resY_{};
    int lastPx_{}, lastPy_{};
    float lastYaw_{}, lastPitch_{};
    bedrock::sdk::Vec3Value aimSmooth_{};
    bool aimSmoothInit_{};
    bedrock::sdk::WorldRender outline_;
    bool outlineReady_{};
    bedrock::sdk::WorldRenderSubscription renderSub_;
    std::mutex pathMutex_;
    std::vector<bedrock::sdk::Vec3Value> pathMarkers_;
    float pathBreakProgress_{-1.f};
    std::vector<bedrock::sdk::Vec3Value> pathWalk_, pathDigLine_, pathBridgeLine_;
    bool pathBox_{};
    BlockPos3 pathBoxPos_{};
    bool showPath_{true};
    bool probePending_{false};
    bool skipUnderwater_{true};
    int submergedTicks_{0};

    bedrock::sdk::Vec3Value lastDryPos_{};
    bool lastDryValid_{};
    bool floodHold_{};
    int floodWetTicks_{};
    int floodDryTicks_{};
    int joinGuardTicks_{}, joinDryTicks_{};
    bool joinGuardNoted_{};
    bool joinArmPending_{true};
    bool joinSurfaced_{};
    std::string lastIdleScreen_;
    bedrock::sdk::Vec3Value lastAlivePos_{};
    bool hasLastAlivePos_{};
    int deathSigTicks_{};
    bool deathPending_{}, hasDeathPos_{};
    bedrock::sdk::Vec3Value deathPos_{};
    bool recoverActive_{};
    int recoverTicks_{}, recoverSweepTicks_{}, recoverStuckTicks_{};
    bedrock::sdk::Vec3Value recoverLastPos_{};
    int escapeTicks_{0};
    int dryTicks_{0};

    std::vector<HazardZone> hazardZones_;
    std::vector<BlockPos3> path_;
    std::vector<bool> pathDig_;
    std::vector<bool> pathBridge_;
    std::vector<bool> pathParkour_;
    std::vector<bool> pathPillar_;
    int pillarWpTicks_{};
    int pillarLandingTicks_{};
    int pillarWpBaseY_{};
    std::map<BlockPos3, std::string> placedPillars_;
    std::string collectDropId_;
    int collectBase_{};
    BlockPos3 collectTarget_{INT_MIN, 0, 0};
    bedrock::sdk::Vec3Value collectEntityPos_{};
    bool collectEntityValid_{};
    int collectStableTicks_{};
    int collectLandingTicks_{};
    int collectNextScan_{};
    int collectNextPlan_{};
    bool collectArrivalReported_{};
    std::optional<BlockPos3> collectDig_;
    std::set<BlockPos3> collectDigFailed_;
    int collectFailAttempts_{};
    int collectAttemptTicks_{};
    BlockPos3 pickupTarget_{INT_MIN, 0, 0};
    std::string pickupItem_;
    int pickupBase_{};
    int bridgeTicks_{};
    int bridgeFailStreak_{};
    BlockPos3 bridgeFailFor_{INT_MIN, 0, 0};
    int bridgeJumpTicks_{};
    double bridgeLastX_{1e18};
    double bridgeLastZ_{};
    int bridgeStuckTicks_{};
    int planSpares_{};
    std::set<BlockPos3> bridgeBlocked_;
    std::size_t pathIdx_{0};
    bool pathDigResume_{};
    bool pathPartial_{};
    int jumpHoldTicks_{};
    int jumpBaseY_{INT_MIN};
    bool sprintWasOn_{};
    std::unordered_set<CellKey, CellKeyHash> planBlocked_;
    BlockPos3 planBlockedFor_{INT_MIN, 0, 0};
    BlockPos3 pathGoal_{INT_MIN, 0, 0};
    BlockPos3 pathFor_{INT_MIN, 0, 0};
    BlockPos3 planFailFor_{INT_MIN, 0, 0};
    int planFailTick_ = -100000;
    int replans_{0};
    int swimSubmerged_{0};
    bool surfacing_{false};
    bool wasInWater_{false};
    Phase phase_{Phase::Idle};
    std::string targetId_;
    BlockPos3 target_{};
    bool hasTarget_{};
    std::set<BlockPos3> blacklist_;
    int mined_{};
    int goalCount_{};
    int goalBase_{};
    int goalProgress_{};
    std::string goalItem_;
    int scanCursor_{};
    bool scanExcludedMatch_{};
    double bestDist2_{1e18};
    int emptyScans_{};
    int escapeStreak_{};
    int scanWait_{};
    int navTicks_{}, stuckTicks_{};
    float lastX_{}, lastZ_{};
    BlockPos3 progressGoal_{INT_MIN, 0, 0};
    double progressBest_{};
    int progressTick_{}, progressMined_{};
    int mineTicks_{}, needTicks_{};
    std::vector<BlockPos3> veinPlan_;
    std::string veinId_;
    BlockPos3 subTarget_{};
    std::string subId_;
    BlockPos3 savedTarget_{};
    std::string savedId_;

    std::vector<std::string> scanIds_;
    std::string scanFoundId_;
    std::string scanSavedId_;
    bool scanOverrideHeld_{};

    bool gatherActive_{}, gatherDone_{}, gatherFailed_{}, gatherHunt_{};
    std::vector<std::string> gatherCountIds_;
    int gatherWant_{};
    long long gatherDeadline_{};

    BlockPos3 digPos_{INT_MIN, 0, 0};
    std::string digBlockId_;
    int digTicks_{};
    bool digLavaChecked_{};
    int smeltBreakRetries_{};

    int descendTargetY_{};
    bool descendDone_{}, descendFailed_{}, descendCave_{};
    int descendCaveRetries_{};
    bool descendResumePending_{};
    int descendDropNoteY_{INT_MAX};
    int descendWetTicks_{}, descendBreathTicks_{}, descendBreathBreaks_{};
    bool descendBreath_{};
    int cobbleNavFails_{};
    bool cobbleDescend_{};
    int descendLastY_{INT_MAX}, descendStuck_{};
    bool descendWide_{};
    int descendCenterTicks_{};
    int descendEntryY_{INT_MAX};

    int lastSurfaceY_{INT_MAX};
    bool descendPending_{};
    bool descendDivertUsed_{};
    int ascendTicks_{};
    int ascendLastY_{INT_MAX};
    int ascendStuck_{};
    int ascendPullTicks_{};
    int ascendTargetY_{INT_MAX};
    bool ascendOpenSky_{};
    int ascendNoteY_{INT_MIN};
    bool ascendDig_{};
    bool scaffoldActive_{};
    bool scaffoldDig_{};
    BlockPos3 scaffoldFor_{INT_MIN, 0, 0};
    std::string scaffoldId_;
    int scaffoldTicks_{};
    int scaffoldPullTicks_{};
    int scaffoldRiseTick_{};
    double scaffoldLastY_{};
    bool scaffoldCenterActive_{};
    bool scaffoldCenterGiveUp_{};
    BlockPos3 scaffoldCenterCell_{};
    int scaffoldCenterTicks_{};
    bool ascendCenterActive_{};
    bool ascendCenterGiveUp_{};
    BlockPos3 ascendCenterCell_{};
    int ascendCenterTicks_{};
    int ascendLastRiseTick_{};
    int ascendShifts_{};
    bool ascendActive_{};
    int ascendDirX_{1};
    int ascendDirZ_{0};
    int ascendStepTicks_{};
    bool ascendStairLatch_{};
    int ascendOpenTicks_{};
    int ascendDirFails_{};
    int ascendStepY_{INT_MAX};
    bool ascendShiftActive_{};
    BlockPos3 ascendShiftCell_{};
    BlockPos3 descendShaftCell_{INT_MIN, 0, 0};
    int ascendShaftTicks_{};
    double ascendShaftBestDist_{1e18};

    int huntDirX_{1}, huntDirZ_{};
    BlockPos3 huntStep_{};
    bool huntStepActive_{};
    int huntStage_{}, huntCells_{}, huntTicks_{};
    bool huntLegDone_{}, huntBlocked_{};

    SmeltStage smeltStage_{SmeltStage::CraftFurnace};
    std::string smeltItem_, smeltYieldItem_;
    int smeltCount_{}, smeltMoved_{}, smeltYieldBase_{}, smeltTicks_{};
    int smeltFuelBurnTicks_{}, smeltFuelPendingBurn_{}, smeltFuelBurnPer_{}, smeltExpect_{};
    BlockPos3 smeltPlaceCell_{INT_MIN, 0, 0}, smeltFurnacePos_{};
    BlockPos3 smeltPocket_{INT_MIN, 0, 0};
    int smeltPocketBad_{};
    int smeltPocketStage_{};
    bool smeltPocketing_{};
    bool smeltPocketDug_{};
    BlockPos3 smeltPocketCell_{INT_MIN, 0, 0};
    int smeltNoSlotTicks_{};
    bool smeltPlaceArmed_{};
    std::size_t smeltPlaceSlotIdx_{};
    int smeltPlaceSlotTicks_{};
    int smeltNoSlotRetries_{};
    std::vector<BlockPos3> smeltPlaceCells_;
    std::size_t smeltPlaceIdx_{};
    int smeltCellTicks_{};
    bool smeltPlaced_{}, smeltTransferStarted_{}, smeltUseAltCont_{};
    int smeltAimedTick_{-1}, smeltCollectTries_{};
    bool furnaceChainTried_{}, smeltDone_{}, smeltFailed_{};
    bool smeltFurnaceGhost_{};
    std::string smeltFailReason_;

    bool forageActive_{};
    int forageWant_{3};
    ForageStage forageStage_{ForageStage::Hunt};
    std::uint32_t forageEid_{}, forageType_{};
    bedrock::sdk::Vec3Value forageCentre_{};
    bool forageEngaged_{};
    int forageTicks_{}, forageKills_{}, forageCollectTicks_{}, forageNoAnimalTicks_{};
    int forageLastHitTick_{};
    std::map<std::uint32_t, int> forageIgnore_;
    bool forageSmeltStarted_{};
    std::set<std::string> forageTriedRaw_;
    std::string forageRawId_, forageCookedId_;
    int forageEatStep_{}, forageEatTicks_{}, forageEatBase_{}, forageEaten_{};

    ExpStage expStage_{ExpStage::None};
    int expNeedTier_{}, expGoalCount_{};
    std::string expGoalItem_;
    int smeltRetries_{};
};

} // namespace automine::client
