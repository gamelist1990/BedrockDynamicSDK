#include <bedrock/packets/packet_api.hpp>
#include <bedrock/sdk/keybinds.hpp>
#include <bedrock/packets/player_roster_service.hpp>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <bedrock/render/gui_overlay.hpp>
#include <bedrock/logging/logger.hpp>
#include <bedrock/detail/hook_dispatch.hpp>
#include <bedrock/native/game_access.hpp>
#include <bedrock/profiles/profile_manager.hpp>
#include <bedrock/runtime/game_hooks.hpp>
#include <bedrock/enums/game_type.hpp>
#include "client.hpp"
#include "overlay.hpp"
#include "tablist/config.hpp"
#include "tablist/model.hpp"
#include "tablist/update_scheduler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sstream>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dynamic_client {
namespace {

bedrock::NativeGameAccess tabListGameAccess;
bedrock::PacketAPI packets;
// Workers and callback targets are process-resident. Joining threads from
// CRT/DllMain teardown can deadlock on the Windows loader lock.
bedrock::PlayerRosterService& packetPlayers = *new bedrock::PlayerRosterService;
bool packetRosterMode{};
constexpr int tabInputKey = 0x09;
constexpr int tabInputKeyGlfw = 258;

using tab_overlay::PlayerRow;
std::atomic<bool> tabHeld{false};
std::atomic<bool> gameplayActive{false};
std::atomic<void*> levelInstance{nullptr}; // Opaque identity supplied by SDK LevelTick.
std::atomic<std::int64_t> levelHeartbeatMs{0};
struct ActorNameTagEntry {
    void* actor{};
    std::string nameTag;
    unsigned colorOrder = 28;
    std::uint64_t sequence = 0;
    std::string playerName;
};
// Keep only the latest observed color per player for concise transition logs.
// A value of 28 means the current Actor nameTag has no supported color.
std::mutex actorNameTagMutex;
std::unordered_map<std::string, unsigned> observedPlayerColorOrders;
std::unordered_map<void*, ActorNameTagEntry> actorNameTags;
std::mutex murderDetectMutex;
std::unordered_map<std::string, tablist::MurderRole> murderRolesByPlayer;
std::unordered_set<std::string> spectatorPlayers;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> murderRoleExpiryByPlayer;
std::unordered_map<std::string, std::string> lastHeldItemByPlayer;
std::unordered_map<std::string, std::string> heldItemNamesByPlayer;
struct MurderSample {
    std::string name;
    std::string identifier;
    std::string displayName;
    std::uint64_t worldEpoch{};
    unsigned count{};
    bool hasItem{};
    bool handEmpty{};
    bool hasGameType{};
    bool spectator{};
};
std::mutex murderQueueMutex;
std::condition_variable murderQueueReady;
std::deque<MurderSample> murderQueue;
std::thread& murderWorker = *new std::thread;
bool murderWorkerStopping{};
std::atomic<std::uint64_t> droppedMurderSamples{0};
std::atomic<std::uint64_t> murderWorldEpoch{0};
bool murderRoundHasRole = false; // Game thread only.
// Raw handles come exclusively from Level::getRuntimeActorList on the game thread.
// Each handle is validated incrementally before use, rather than in one sweep.
struct MurderActorEntry {
    void* actor{};
    std::uint64_t runtimeId{};
    std::uintptr_t scanKey{};
    std::string playerName;
};
std::vector<MurderActorEntry> murderScanActors;
std::vector<std::string> murderPlayerNames;
std::unordered_map<std::uint64_t, std::string> murderKnownPlayerNamesByRuntimeId;
std::unordered_map<void*, std::string> murderKnownPlayerNamesByActor;
std::size_t murderScanCursor = 0;
std::uintptr_t murderLastProcessedScanKey = 0;
void* murderScanLevel = nullptr;
struct ActorRosterBuild {
    std::vector<void*> handles;
    std::size_t cursor{};
    std::vector<MurderActorEntry> tracked;
    std::unordered_set<std::uint64_t> runtimeIds;
    std::unordered_map<std::uint64_t, std::string> namesById;
    std::unordered_map<void*, std::string> namesByActor;
};
std::optional<ActorRosterBuild> pendingActorRoster;
std::int64_t nextActorRosterMs{};
std::int64_t lastMurderRosterStatusLogMs{};
std::atomic<bool> loggedMurderActorReadFault{false};

void pruneExpiredMurderRolesLocked(std::chrono::steady_clock::time_point now) {
    for (auto it = murderRoleExpiryByPlayer.begin(); it != murderRoleExpiryByPlayer.end();) {
        if (it->second > now) {
            ++it;
            continue;
        }
        murderRolesByPlayer.erase(it->first);
        it = murderRoleExpiryByPlayer.erase(it);
    }
}
std::size_t cachedFaceCursor = 0;
std::mutex cachedTabRowsWriteMutex;
std::atomic<std::shared_ptr<const std::vector<PlayerRow>>> cachedTabRows{
    std::make_shared<const std::vector<PlayerRow>>()};
struct TabRowsBuild {
    bool failed{};
    std::uint64_t worldEpoch{};
    std::vector<std::string> names;
    std::vector<PlayerRow> rows;
    std::shared_ptr<const tablist::Config> config;
    std::unordered_map<std::string, tablist::MurderRole> roles;
    std::unordered_set<std::string> spectators;
    std::vector<ActorNameTagEntry> actorTags;
};
std::mutex tabRowsWorkerMutex;
std::thread& tabRowsWorker = *new std::thread;
std::condition_variable tabRowsReady;

class TabThreadPool {
public:
    explicit TabThreadPool(std::size_t count) {
        for (std::size_t i = 0; i < count; ++i)
            threads_.emplace_back(&TabThreadPool::run, this);
    }
    void enqueue(std::function<void()> task) {
        {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(task));
        }
        condition_.notify_one();
    }
private:
    void run() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                if (stopping_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            try { task(); } catch (...) {}
        }
    }
    std::vector<std::thread> threads_;
    std::deque<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stopping_{};
};
TabThreadPool& tabThreadPool = *new TabThreadPool(4);
std::shared_ptr<TabRowsBuild> pendingTabRowsBuild;
struct FaceUpdate {
    std::uint64_t worldEpoch{};
    PlayerRow row;
};
std::optional<FaceUpdate> pendingFaceUpdate;
bool tabRowsStopping{};
std::unordered_map<std::string, std::array<std::byte,16>> playerFaceIds; // Game thread only.
std::int64_t nextFaceReadMs{};
std::int64_t tabRowsLastStartedMs{};
std::atomic<bool> loggedTabRowsReady{false};
std::atomic<std::uint64_t> actorNameTagSequence{0};
std::optional<std::uint64_t> readActorRuntimeId(void* actor) noexcept {
    return tabListGameAccess.actorRuntimeId(actor);
}
std::unique_ptr<bedrock::detail::HookDispatch> eventDispatch;
std::unique_ptr<bedrock::GameHooks> gameHooks;
std::atomic<std::shared_ptr<const tablist::Config>> liveConfig{
    std::make_shared<const tablist::Config>()};
// Callbacks enqueue text; only this writer thread opens/writes/flushes the file.
class DiagnosticWriter {
public:
    bool start(const std::filesystem::path& path) noexcept {
        try {
            writer_ = std::thread([this, path] {
                try {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                    std::ofstream output(path, std::ios::binary | std::ios::trunc);
                    for (;;) {
                        std::deque<std::string> batch;
                        bool done;
                        {
                            std::unique_lock lock(mutex_);
                            ready_.wait_for(lock, std::chrono::milliseconds(250),
                                [this] { return stopping_; });
                            batch.swap(lines_);
                            done = stopping_;
                        }
                        for (const auto& line : batch) output << line << '\n';
                        if (!batch.empty()) output.flush();
                        if (done) return;
                    }
                } catch (...) {}
            });
            return true;
        } catch (...) { return false; }
    }
    void enqueue(std::string line) noexcept {
        try {
            std::unique_lock lock(mutex_, std::try_to_lock);
            if (lock && lines_.size() < 512) lines_.push_back(std::move(line));
        } catch (...) {}
    }
    void stop() noexcept {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_one();
        if (writer_.joinable()) writer_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::string> lines_;
    std::thread writer_;
    bool stopping_{};
};
DiagnosticWriter& diagnosticWriter = *new DiagnosticWriter;

template <class... Args>
void diagnostic(Args&&... args) noexcept {
    try {
        std::ostringstream line;
        (line << ... << std::forward<Args>(args));
        diagnosticWriter.enqueue(std::move(line).str());
    } catch (...) {}
}

void publishHeldItemName(const std::string& playerName,
                         const std::string& displayName,
                         std::uint64_t worldEpoch) noexcept {
    try {
        std::lock_guard writeLock(cachedTabRowsWriteMutex);
        if (worldEpoch != murderWorldEpoch.load(std::memory_order_acquire)) return;
        const auto current = cachedTabRows.load(std::memory_order_acquire);
        if (!current || current->empty()) return;
        const auto found = std::find_if(current->begin(), current->end(),
            [&](const PlayerRow& row) { return row.name == playerName; });
        if (found == current->end() || found->heldItemName == displayName) return;
        auto updated = std::make_shared<std::vector<PlayerRow>>(*current);
        (*updated)[static_cast<std::size_t>(std::distance(current->begin(), found))]
            .heldItemName = displayName;
        cachedTabRows.store(std::shared_ptr<const std::vector<PlayerRow>>(std::move(updated)),
            std::memory_order_release);
    } catch (...) {
        // Rendering remains on the last immutable row snapshot on allocation failure.
    }
}

std::string normalizeItemDisplayName(std::string_view raw,
                                     std::string_view fallback) {
    auto name = tablist::stripMinecraftFormatting(raw);
    const auto actionHint = name.find('[');
    if (actionHint != std::string::npos) name.resize(actionHint);
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
        name.pop_back();
    const auto first = std::find_if_not(name.begin(), name.end(),
        [](unsigned char c) { return std::isspace(c) != 0; });
    name.erase(name.begin(), first);
    if (name.empty()) name.assign(fallback);

    // Keep complete UTF-8 code points in the narrow TabList row.
    std::size_t offset = 0;
    std::size_t codepoints = 0;
    while (offset < name.size() && codepoints < 40) {
        const auto lead = static_cast<unsigned char>(name[offset]);
        const std::size_t width = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2
            : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 1;
        offset += std::min(width, name.size() - offset);
        ++codepoints;
    }
    if (offset < name.size()) {
        name.resize(offset);
        name += "...";
    }
    return name;
}

void processMurderSample(const MurderSample& sample) noexcept {
    try {
        if (sample.worldEpoch != murderWorldEpoch.load(std::memory_order_acquire)) return;
        std::string itemKey = sample.identifier.empty()
            ? sample.displayName : sample.identifier;
        if (itemKey.size() > 256) return;
        std::transform(itemKey.begin(), itemKey.end(), itemKey.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto sampleConfig = liveConfig.load(std::memory_order_acquire);
        const auto role = sample.hasItem && sampleConfig->murderDetect
            ? tablist::murderRoleForHeldItem(sample.identifier, sample.displayName)
            : tablist::MurderRole::None;

        bool changed = false;
        std::string previous;
        std::string currentDisplay;
        bool clearedHeldItem = false;
        {
            std::lock_guard lock(murderDetectMutex);
            if (sample.worldEpoch != murderWorldEpoch.load(std::memory_order_acquire))
                return;
            const auto now = std::chrono::steady_clock::now();
            const auto old = lastHeldItemByPlayer.find(sample.name);
            if (old != lastHeldItemByPlayer.end()) previous = old->second;
            // A confirmed empty slot clears stale item state; failed reads
            // leave the prior snapshot untouched.
            if (sample.handEmpty) {
                clearedHeldItem = old != lastHeldItemByPlayer.end() ||
                                  heldItemNamesByPlayer.contains(sample.name);
                lastHeldItemByPlayer.erase(sample.name);
                heldItemNamesByPlayer.erase(sample.name);
            } else if (!itemKey.empty()) {
                changed = old == lastHeldItemByPlayer.end() || old->second != itemKey;
                lastHeldItemByPlayer.insert_or_assign(sample.name, itemKey);
            }
            if (sample.hasItem && sampleConfig && sampleConfig->showHeldItems) {
                std::string display = normalizeItemDisplayName(
                    sample.displayName, sample.identifier);
                if (!display.empty()) {
                    currentDisplay = std::move(display);
                    heldItemNamesByPlayer.insert_or_assign(sample.name, currentDisplay);
                }
            }
            if (role != tablist::MurderRole::None) {
                murderRolesByPlayer.insert_or_assign(sample.name, role);
                murderRoleExpiryByPlayer.insert_or_assign(
                    sample.name, now + std::chrono::minutes(5));
            }
            if (sample.hasGameType) {
                if (sample.spectator) spectatorPlayers.insert(sample.name);
                else spectatorPlayers.erase(sample.name);
            }
        }
        if (sample.worldEpoch != murderWorldEpoch.load(std::memory_order_acquire)) return;
        if (!currentDisplay.empty() || clearedHeldItem)
            publishHeldItemName(sample.name, currentDisplay, sample.worldEpoch);
        if (changed && sample.hasItem) {
            const char* roleName = role == tablist::MurderRole::Sheriff ? "Sheriff"
                : role == tablist::MurderRole::Murderer ? "Murderer" : "none";
            const bool resolvedPlayer = sample.name.rfind("unmatched@0x", 0) != 0;
            diagnostic("Item held: ", resolvedPlayer ? "player=" : "actor=",
                       sample.name,
                       " name=", currentDisplay.empty() ? itemKey : currentDisplay,
                       " id=", itemKey, " count=", sample.count,
                       " role=", roleName,
                       previous.empty() ? "" : " previous=", previous);
        }
    } catch (...) {
        // Background reporting must not terminate the worker or affect the game.
    }
}

void murderWorkerLoop() noexcept {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        MurderSample sample;
        {
            std::unique_lock lock(murderQueueMutex);
            murderQueueReady.wait(lock, [] {
                return murderWorkerStopping || !murderQueue.empty();
            });
            if (murderWorkerStopping && murderQueue.empty()) return;
            sample = std::move(murderQueue.front());
            murderQueue.pop_front();
        }
        processMurderSample(sample);
        const auto dropped = droppedMurderSamples.exchange(0, std::memory_order_relaxed);
        if (dropped) diagnostic("MurderDetect worker queue dropped ", dropped,
                                " older sample(s)");
    }
}

bool startMurderWorker() noexcept {
    try {
        std::lock_guard lock(murderQueueMutex);
        if (murderWorker.joinable()) return true;
        murderWorkerStopping = false;
        murderWorker = std::thread(murderWorkerLoop);
        return true;
    } catch (...) {
        return false;
    }
}

void stopMurderWorker() noexcept {
    {
        std::lock_guard lock(murderQueueMutex);
        if (!murderWorker.joinable()) return;
        murderWorkerStopping = true;
    }
    murderQueueReady.notify_all();
    murderWorker.join();
    std::lock_guard lock(murderQueueMutex);
    murderWorkerStopping = false;
}

void enqueueMurderSample(MurderSample sample) noexcept {
    try {
        if (sample.worldEpoch != murderWorldEpoch.load(std::memory_order_acquire)) return;
        std::unique_lock lock(murderQueueMutex, std::try_to_lock);
        if (!lock || !murderWorker.joinable() || murderWorkerStopping) {
            droppedMurderSamples.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        constexpr std::size_t maxQueuedMurderSamples = 256;
        if (murderQueue.size() == maxQueuedMurderSamples) {
            murderQueue.pop_front();
            droppedMurderSamples.fetch_add(1, std::memory_order_relaxed);
        }
        murderQueue.push_back(std::move(sample));
        lock.unlock();
        murderQueueReady.notify_one();
    } catch (...) {
        // Never run role processing, row copies or logging inline on queue failure.
        droppedMurderSamples.fetch_add(1, std::memory_order_relaxed);
    }
}

std::int64_t steadyMilliseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool levelIsFresh() noexcept {
    const auto heartbeat = levelHeartbeatMs.load(std::memory_order_acquire);
    return heartbeat > 0 && steadyMilliseconds() - heartbeat <= 1000;
}

void recordLevel(void* level, bool lobbyReturn = false) noexcept {
    if (!level) return;
    static tablist::WorldIdentityTracker identity;
    auto* player = tabListGameAccess.localPlayer();
    const bool changed = identity.observe(reinterpret_cast<std::uintptr_t>(level),
        reinterpret_cast<std::uintptr_t>(player), readActorRuntimeId(player));
    levelInstance.store(level, std::memory_order_release);
    levelHeartbeatMs.store(steadyMilliseconds(), std::memory_order_release);
    if (changed || lobbyReturn) {
        murderWorldEpoch.fetch_add(1, std::memory_order_acq_rel);
        murderRoundHasRole = false;
        tabListGameAccess.clearWorldCaches();
        if (eventDispatch) eventDispatch->clearObservedActors();
        tabRowsLastStartedMs = 0;
        {
            std::lock_guard rowsLock(tabRowsWorkerMutex);
            pendingTabRowsBuild.reset();
            pendingFaceUpdate.reset();
        }
        {
            std::lock_guard queueLock(murderQueueMutex);
            murderQueue.clear();
        }
        {
            std::lock_guard detectLock(murderDetectMutex);
            murderRolesByPlayer.clear();
            spectatorPlayers.clear();
            murderRoleExpiryByPlayer.clear();
            lastHeldItemByPlayer.clear();
            heldItemNamesByPlayer.clear();
        }
        playerFaceIds.clear();

        murderScanActors.clear();
        murderPlayerNames.clear();
        murderKnownPlayerNamesByRuntimeId.clear();
        murderKnownPlayerNamesByActor.clear();
        pendingActorRoster.reset();
        murderScanLevel = nullptr;
        murderScanCursor = 0;
        murderLastProcessedScanKey = 0;
        nextActorRosterMs = 0;
        nextFaceReadMs = 0;
        cachedFaceCursor = 0;
        {
            std::lock_guard rowsLock(cachedTabRowsWriteMutex);
            cachedTabRows.store(std::shared_ptr<const std::vector<PlayerRow>>{}, std::memory_order_release);
        }
        loggedTabRowsReady.store(false, std::memory_order_release);
        {
            std::lock_guard lock(actorNameTagMutex);
            observedPlayerColorOrders.clear();
            actorNameTags.clear();
        }
        loggedMurderActorReadFault.store(false, std::memory_order_release);
        droppedMurderSamples.store(0, std::memory_order_release);
        diagnostic("level ready [", lobbyReturn ? "returned to lobby" : "world identity changed",
                   "; roles and item caches reset]");
    }
}

void captureQueriedActorNameTag(void* actor, const std::string& nameTag) noexcept {
    if (!actor || nameTag.size() > 512) return;
    try {
        // Called by the renderer's native getter: never wait for a snapshot copy.
        std::unique_lock lock(actorNameTagMutex, std::try_to_lock);
        if (!lock) return;
        const auto existing = actorNameTags.find(actor);
        if (existing != actorNameTags.end() && existing->second.nameTag == nameTag) return;
        if (existing == actorNameTags.end() && actorNameTags.size() >= 4096)
            actorNameTags.erase(actorNameTags.begin());
        const auto sequence = actorNameTagSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        actorNameTags.insert_or_assign(actor,
            ActorNameTagEntry{actor, nameTag, 28, sequence});
    } catch (...) {}
}

// Only copied names cross to the worker. Handles are opaque comparison keys there.
std::vector<ActorNameTagEntry> snapshotActorNameTags() {
    std::vector<ActorNameTagEntry> result;
    std::lock_guard lock(actorNameTagMutex);
    result.reserve(actorNameTags.size());
    for (const auto& [_, tag] : actorNameTags) result.push_back(tag);
    return result;
}

std::string matchPlayerNameFromTag(
    std::string_view nameTag, const std::vector<std::string>& playerNames) {
    const auto plainTag = tablist::stripMinecraftFormatting(nameTag);
    for (const auto& playerName : playerNames) {
        if (playerName.size() > plainTag.size()) continue;
        for (std::size_t offset = 0; offset + playerName.size() <= plainTag.size(); ++offset) {
            bool matches = true;
            for (std::size_t i = 0; i < playerName.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(plainTag[offset + i])) !=
                    std::tolower(static_cast<unsigned char>(playerName[i]))) {
                    matches = false;
                    break;
                }
            }
            if (matches) return playerName;
        }
    }
    return {};
}

bool collectRowsUnsafe(std::vector<PlayerRow>& rows,
                       const std::vector<std::string>& playerNames,
                       std::size_t begin, std::size_t end,
                       bool sortByNameColor, bool gamemodeCheck,
                       bool murderDetect,
                       bool debug,
                       const std::unordered_map<std::string, tablist::MurderRole>& murderRoles,
                       const std::unordered_set<std::string>& spectators,
                       const std::vector<ActorNameTagEntry>& actorTags) {
    if (begin > end || end > playerNames.size()) return false;
    for (std::size_t index = begin; index < end; ++index) {
        const auto& nativeName = playerNames[index];
        const auto length = nativeName.size();
        if (length == 0 || length > 128) continue;
        PlayerRow row;
        row.name = tablist::stripMinecraftFormatting(nativeName);
        if (row.name.empty()) continue;
        if (murderDetect) {
            const auto role = murderRoles.find(row.name);
            if (role != murderRoles.end()) row.murderRole = role->second;
        }
        row.spectator = gamemodeCheck && spectators.contains(row.name);
        if (sortByNameColor) {
            bool foundColor = false;
            bool bestWasExact = false;
            std::uint64_t bestSequence = 0;
            for (const auto& actorTag : actorTags) {
                const auto color = tablist::actorNameColorForPlayerName(row.name, actorTag.nameTag);
                if (!color) continue;
                const bool exact = true;
                if (foundColor && actorTag.sequence <= bestSequence) continue;
                row.nameColorOrder = *color;
                row.hasNameColor = true;
                bestWasExact = exact;
                bestSequence = actorTag.sequence;
                foundColor = true;
            }
            // Record every rendered row, including loss of a color. This makes
            // subsequent Actor::getNameTag changes visible without reopening
            // the TabList, while avoiding per-frame/per-Actor log spam.
            bool colorChanged = false;
            unsigned oldOrder = 28;
            unsigned newOrder = foundColor ? row.nameColorOrder : 28u;
            {
                std::lock_guard lock(actorNameTagMutex);
                const auto it = observedPlayerColorOrders.find(row.name);
                if (it == observedPlayerColorOrders.end()) {
                    observedPlayerColorOrders.emplace(row.name, newOrder);
                    colorChanged = foundColor;
                } else if (it->second != newOrder) {
                    oldOrder = it->second;
                    it->second = newOrder;
                    colorChanged = true;
                }
            }
            if (colorChanged && debug) {
                const auto colorLabel = [](unsigned order) {
                    constexpr std::string_view codes = "0123456789abcdefghijmnpqstuv";
                    if (order == 10) return 'p'; // target server's §p green
                    return order < codes.size() ? codes[order] : '-';
                };
                const auto colorName = [](unsigned order) -> std::string_view {
                    static constexpr std::array<std::string_view, 28> names{
                        "black", "dark_blue", "dark_green", "dark_aqua",
                        "dark_red", "dark_purple", "orange", "gray", "dark_gray",
                        "blue", "green", "aqua", "red", "light_purple", "yellow",
                        "white", "minecoin_gold", "material_quartz", "server_purple",
                        "material_netherite", "material_redstone", "material_copper",
                        "material_gold", "material_emerald", "material_diamond",
                        "material_lapis", "material_amethyst", "material_resin"};
                    return order < names.size() ? names[order] : "none";
                };
                diagnostic("PlayerList name color updated: player=", row.name,
                           " ", colorLabel(oldOrder), "->", colorLabel(newOrder),
                           " (", colorName(newOrder), "; match=",
                           bestWasExact ? "exact" : "none", ")");
            }
        }
        rows.push_back(std::move(row));
    }
    return true;
}

bool copyPlayerNamesUnsafe(std::vector<std::string>& names) {
    void* level = levelInstance.load(std::memory_order_acquire);
    if (!level || !eventDispatch) return false;
    const auto players = tabListGameAccess.rosterSnapshot(level);
    names.clear(); playerFaceIds.clear();
    for (const auto& player : players) {
        names.push_back(player.name);
        playerFaceIds.insert_or_assign(tablist::stripMinecraftFormatting(player.name),player.uuid);
    }
    return names.size() <= 512;
}

bool copyPlayerNames(std::vector<std::string>* names) noexcept {
    if (!names) return false;
    try {
        return copyPlayerNamesUnsafe(*names);
    } catch (...) {
        names->clear();
        return false;
    }
}

bool collectRowsChunk(std::vector<PlayerRow>& rows,
                 const std::vector<std::string>& playerNames,
                 std::size_t begin, std::size_t end,
                 bool sortByNameColor, bool gamemodeCheck,
                 bool murderDetect, bool debug,
                 const std::unordered_map<std::string, tablist::MurderRole>& murderRoles,
                 const std::unordered_set<std::string>& spectators,
                 const std::vector<ActorNameTagEntry>& actorTags) noexcept {
    // Only owned SDK snapshots are processed here, including on the worker.
    try {
        return collectRowsUnsafe(rows, playerNames, begin, end, sortByNameColor, gamemodeCheck, murderDetect,
                                 debug, murderRoles, spectators, actorTags);
    } catch (...) {
        rows.clear();
        return false;
    }
}

void buildTabRowsInBackground(const std::shared_ptr<TabRowsBuild>& build) noexcept {
    const auto buildStarted = std::chrono::steady_clock::now();
    try {
        if (build->config->sortByNameColor) build->actorTags = snapshotActorNameTags();
        {
            std::lock_guard lock(murderDetectMutex);
            pruneExpiredMurderRolesLocked(std::chrono::steady_clock::now());
            build->roles = murderRolesByPlayer;
            build->spectators = spectatorPlayers;
        }
        const auto totalNames = build->names.size();
        const std::size_t chunkCount = std::min<std::size_t>(4, totalNames == 0 ? 1 : totalNames);
        std::vector<std::vector<PlayerRow>> chunkRows(chunkCount);
        std::vector<char> chunkOk(chunkCount, 1);
        std::atomic<std::size_t> remaining{chunkCount};
        std::mutex doneMutex;
        std::condition_variable doneCv;
        for (std::size_t chunk = 0; chunk < chunkCount; ++chunk) {
            const std::size_t begin = totalNames * chunk / chunkCount;
            const std::size_t end = totalNames * (chunk + 1) / chunkCount;
            tabThreadPool.enqueue([&, chunk, begin, end]() {
                const bool ok = collectRowsChunk(chunkRows[chunk], build->names, begin, end,
                    build->config->sortByNameColor, build->config->gamemodeCheck,
                    build->config->murderDetect, build->config->debug,
                    build->roles, build->spectators, build->actorTags);
                chunkOk[chunk] = ok ? 1 : 0;
                if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    std::lock_guard lock(doneMutex);
                    doneCv.notify_one();
                }
            });
        }
        {
            std::unique_lock lock(doneMutex);
            doneCv.wait(lock, [&] { return remaining.load(std::memory_order_acquire) == 0; });
        }
        for (std::size_t chunk = 0; chunk < chunkCount; ++chunk) {
            if (!chunkOk[chunk]) {
                build->failed = true;
                break;
            }
        }
        build->rows.clear();
        if (!build->failed) {
            build->rows.reserve(totalNames);
            for (auto& chunk : chunkRows) {
                std::move(chunk.begin(), chunk.end(), std::back_inserter(build->rows));
            }
        }
        if (!build->failed) {
            const auto config = build->config;
            std::stable_sort(build->rows.begin(), build->rows.end(),
                [config](const auto& left, const auto& right) {
                    if (config->sortByNameColor &&
                        left.nameColorOrder != right.nameColorOrder)
                        return left.nameColorOrder < right.nameColorOrder;
                    return std::lexicographical_compare(
                        left.name.begin(), left.name.end(),
                        right.name.begin(), right.name.end(),
                        [](unsigned char a, unsigned char b) {
                            return std::tolower(a) < std::tolower(b);
                        });
                });
            build->rows.erase(std::unique(build->rows.begin(), build->rows.end(),
                [](const auto& left, const auto& right) {
                    return left.name == right.name;
                }), build->rows.end());
        }
    } catch (...) {
        build->failed = true;
    }
    if (build->failed) return;
    try {
        const auto config = build->config;
        std::lock_guard writeLock(cachedTabRowsWriteMutex);
        if (build->worldEpoch != murderWorldEpoch.load(std::memory_order_acquire) ||
            config != liveConfig.load(std::memory_order_acquire)) return;
        {
            std::lock_guard stateLock(murderDetectMutex);
            for (auto& row : build->rows) {
                if (config->murderDetect) {
                    const auto role = murderRolesByPlayer.find(row.name);
                    if (role != murderRolesByPlayer.end()) row.murderRole = role->second;
                }
                row.spectator = config->gamemodeCheck && spectatorPlayers.contains(row.name);
                if (config->showHeldItems) {
                    const auto item = heldItemNamesByPlayer.find(row.name);
                    if (item != heldItemNamesByPlayer.end()) row.heldItemName = item->second;
                }
            }
        }
        if(packetRosterMode&&config->showFaces){
            const auto players=packetPlayers.snapshotShared();
            std::unordered_map<std::string,const bedrock::PacketPlayer*> faces;
            faces.reserve(players->size());
            for(const auto& player:*players)faces.try_emplace(tablist::stripMinecraftFormatting(player.name),&player);
            for(auto& row:build->rows){const auto found=faces.find(row.name);if(found!=faces.end()){row.face=found->second->face;row.hasFace=found->second->hasFace;}}
        }
        const auto previous = cachedTabRows.load(std::memory_order_acquire);
        std::unordered_map<std::string_view, const PlayerRow*> oldRows;
        if (config->showFaces && previous && !packetRosterMode) {
            oldRows.reserve(previous->size());
            for (const auto& row : *previous) if (row.hasFace) oldRows.emplace(row.name, &row);
            for (auto& row : build->rows) {
                const auto old = oldRows.find(row.name);
                if (old != oldRows.end()) {
                    row.face = old->second->face;
                    row.hasFace = true;
                }
            }
        }
        const auto publishedCount=build->rows.size();
        const auto buildDurationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - buildStarted).count();
        diagnostic("TabList roster build: ", buildDurationMs, " ms thread=", std::this_thread::get_id(),
                   " rows=", publishedCount);
        cachedTabRows.store(std::make_shared<const std::vector<PlayerRow>>(std::move(build->rows)),
            std::memory_order_release);
        if (!loggedTabRowsReady.exchange(true, std::memory_order_acq_rel))
            diagnostic("TabList row cache ready; rows=",publishedCount," source_names=",build->names.size()," [copied worker rows]");
    } catch (...) {}
}

void publishFaceUpdate(const FaceUpdate& update) {
    std::lock_guard lock(cachedTabRowsWriteMutex);
    if (update.worldEpoch != murderWorldEpoch.load(std::memory_order_acquire)) return;
    const auto previous = cachedTabRows.load(std::memory_order_acquire);
    if (!previous) return;
    const auto found = std::find_if(previous->begin(), previous->end(),
        [&](const PlayerRow& row) { return row.name == update.row.name; });
    if (found == previous->end() || found->hasFace) return;
    auto rows = std::make_shared<std::vector<PlayerRow>>(*previous);
    auto& row = (*rows)[static_cast<std::size_t>(found - previous->begin())];
    row.face = update.row.face;
    row.hasFace = true;
    cachedTabRows.store(std::shared_ptr<const std::vector<PlayerRow>>(std::move(rows)), std::memory_order_release);
}

void tabRowsWorkerLoop() noexcept {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        std::shared_ptr<TabRowsBuild> build;
        std::optional<FaceUpdate> face;
        {
            std::unique_lock lock(tabRowsWorkerMutex);
            tabRowsReady.wait(lock, [] {
                return tabRowsStopping || pendingTabRowsBuild || pendingFaceUpdate;
            });
            if (tabRowsStopping) return;
            build = std::move(pendingTabRowsBuild);
            face = std::move(pendingFaceUpdate);
            pendingFaceUpdate.reset();
        }
        if (build) buildTabRowsInBackground(build);
        try { if (face) publishFaceUpdate(*face); } catch (...) {}
    }
}

bool startTabRowsWorker() noexcept {
    try {
        tabRowsWorker = std::thread(tabRowsWorkerLoop);
        return true;
    } catch (...) { return false; }
}

void stopTabRowsWorker() noexcept {
    {
        std::lock_guard lock(tabRowsWorkerMutex);
        tabRowsStopping = true;
        pendingTabRowsBuild.reset();
        pendingFaceUpdate.reset();
    }
    tabRowsReady.notify_one();
    if (tabRowsWorker.joinable()) tabRowsWorker.join();
}

bool fillOneFaceUnsafe(void* level, PlayerRow* row) {
    if(!level||!row||row->name.empty())return false;
    if(packetRosterMode){const auto players=packetPlayers.snapshotShared();for(const auto& player:*players)
        if(tablist::stripMinecraftFormatting(player.name)==row->name){row->face=player.face;row->hasFace=player.hasFace;return row->hasFace;}
        return false;}
    const auto uuid=playerFaceIds.find(row->name);if(uuid==playerFaceIds.end())return false;
    row->hasFace=tabListGameAccess.face(level,uuid->second,row->face);return row->hasFace;
}

bool captureOneFace(void* level, PlayerRow* row) noexcept {
    try {
        return fillOneFaceUnsafe(level, row);
    } catch (...) {
        return false;
    }
}

void refreshOneCachedFace(const std::shared_ptr<const tablist::Config>& config) noexcept {
    if (!config || !config->showFaces) return;
    nextFaceReadMs = steadyMilliseconds() + 200;
    try {
        const auto current = cachedTabRows.load(std::memory_order_acquire);
        if (!current || current->empty()) return;
        // Retain progress when unrelated item/row publications replace the snapshot.
        for (std::size_t scanned = 0; scanned < current->size(); ++scanned) {
            const auto index = cachedFaceCursor++ % current->size();
            if ((*current)[index].hasFace) continue;
            FaceUpdate update;
            update.worldEpoch = murderWorldEpoch.load(std::memory_order_acquire);
            update.row.name = (*current)[index].name;
            if (!captureOneFace(levelInstance.load(std::memory_order_acquire), &update.row)) return;
            std::unique_lock lock(tabRowsWorkerMutex, std::try_to_lock);
            if (!lock || tabRowsStopping) return;
            pendingFaceUpdate = std::move(update);
            lock.unlock();
            tabRowsReady.notify_one();
            return;
        }
    } catch (...) {}
}

void refreshTabRowsCache(const std::shared_ptr<const tablist::Config>& config) noexcept {
    if (!config || !config->enabled) return;
    try {
        auto next = std::make_shared<TabRowsBuild>();
        next->worldEpoch = murderWorldEpoch.load(std::memory_order_acquire);
        next->config = config;
        if (!copyPlayerNames(&next->names)) return;
        std::unique_lock lock(tabRowsWorkerMutex, std::try_to_lock);
        if (!lock || tabRowsStopping) return;
        // Only the latest captured input matters. No game callback starts or joins a thread.
        // Match Actor names against the same capture used to render the PlayerList.
        murderPlayerNames = next->names;
        for (auto& name : murderPlayerNames) name = tablist::stripMinecraftFormatting(name);
        std::sort(murderPlayerNames.begin(), murderPlayerNames.end(),
            [](const auto& a, const auto& b) {
                return a.size() != b.size() ? a.size() > b.size() : a < b;
            });
        murderPlayerNames.erase(std::remove(murderPlayerNames.begin(), murderPlayerNames.end(), ""),
                                murderPlayerNames.end());
        murderPlayerNames.erase(std::unique(murderPlayerNames.begin(), murderPlayerNames.end()),
                                murderPlayerNames.end());
        if (murderPlayerNames.empty()) {
            murderScanActors.clear();
            pendingActorRoster.reset();
            murderKnownPlayerNamesByRuntimeId.clear();
            murderKnownPlayerNamesByActor.clear();
            nextActorRosterMs = 0;
        }
        pendingTabRowsBuild = std::move(next);
        tabRowsLastStartedMs = steadyMilliseconds();
        lock.unlock();
        tabRowsReady.notify_one();
    } catch (...) {}
}

void onGameplay(const bedrock::GameplayEvent& event) {
    gameplayActive.store(event.active,std::memory_order_release);
    if(!event.active) tabHeld.store(false,std::memory_order_release);
}
struct ActorMurderSnapshot {
    bool isPlayer = false;
    std::string name;
    std::optional<bedrock::ItemStack> item;
    bool handEmpty = false;
    bool hasGameType = false;
    bool spectator = false;
};

void readActorMurderSnapshot(void* actor,
                             std::string_view playerName,
                             ActorMurderSnapshot& snapshot,
                             bool readItem, bool readGameMode) {
    if (!actor) return;
    if (playerName.empty() || playerName.size() > 128) return;
    snapshot.name.assign(playerName);
    snapshot.isPlayer = true;
    if (readItem) {
        auto hand=tabListGameAccess.equipmentSnapshot(actor,bedrock::EquipmentSlot::MainHand);
        snapshot.handEmpty=hand.available && !hand.item;
        snapshot.item=std::move(hand.item);
    }
    if (readGameMode) {
        const auto gameType=tabListGameAccess.gameType(actor);
        snapshot.hasGameType=gameType.has_value();
        snapshot.spectator=gameType==bedrock::GameType::Spectator;
    }
}

// The SDK owns native validation and exception boundaries; this layer only
// combines copied snapshots with the Mod's player-name and role policy.
bool captureActorMurderSnapshot(void* actor,
                                const std::string* playerName,
                                ActorMurderSnapshot* snapshot,
                                bool readItem, bool readGameMode) noexcept {
    if (!actor || !playerName || !snapshot) return false;
    try {
        readActorMurderSnapshot(actor, *playerName, *snapshot, readItem, readGameMode);
        return true;
    } catch (...) {
        return false;
    }
}

void updateMurderDetection(bool readItems, bool readGameMode) noexcept {
    // Keep native Actor/inventory reads thread-affine, but sample a small
    // bounded batch each tick so a brief held-item change is less likely to
    // fall between two samples. Opening the TabList never triggers a scan burst.
    if (!eventDispatch) return;
    try {
        void* level = levelInstance.load(std::memory_order_acquire);
        if (!level) return;
        if (murderScanActors.empty() || murderPlayerNames.empty()) return;
        const auto config=liveConfig.load(std::memory_order_acquire);
        const auto sampleCount=tablist::equipmentReadsPerTick(murderScanActors.size(),config->murderScanIntervalMs);
        const auto begin=std::chrono::steady_clock::now();
        std::size_t attempted=0;
        for (std::size_t sample = 0; sample < sampleCount; ++sample) {
            // A single native call cannot be preempted. Continue from this cursor
            // on the next tick instead of doing overdue work in a catch-up burst.
            if(sample && std::chrono::steady_clock::now()-begin>=std::chrono::microseconds(750)) break;
            ++attempted;
            if (murderScanCursor >= murderScanActors.size()) murderScanCursor = 0;
            const auto& tracked = murderScanActors[murderScanCursor];
            auto* actor = tracked.actor;
            murderScanCursor = (murderScanCursor + 1) % murderScanActors.size();
            const auto runtimeId = tracked.runtimeId;
            // A handle captured across ticks may have been destroyed/reused.
            // Check its identity before reading equipment or trusting the cached name.
            if (runtimeId != 0 && readActorRuntimeId(actor) != std::optional{runtimeId}) continue;
            if (std::find(murderPlayerNames.begin(), murderPlayerNames.end(), tracked.playerName) ==
                murderPlayerNames.end()) continue;
            murderLastProcessedScanKey = tracked.scanKey;
            ActorMurderSnapshot snapshot;
            if (!captureActorMurderSnapshot(actor, &tracked.playerName, &snapshot,
                                            readItems, readGameMode)) {
                if (!loggedMurderActorReadFault.exchange(true, std::memory_order_acq_rel))
                    diagnostic("MurderDetect skipped an Actor whose native read raised an exception");
                continue;
            }
            const auto& item = snapshot.item;
            if ((!snapshot.isPlayer || snapshot.name.empty() || snapshot.name.size() > 128) &&
                !item) continue;
            MurderSample copied;
            copied.worldEpoch = murderWorldEpoch.load(std::memory_order_acquire);
            if (!snapshot.name.empty()) {
                copied.name = std::move(snapshot.name);
            } else {
                std::ostringstream actorLabel;
                actorLabel << "runtimeId=" << std::dec << runtimeId;
                copied.name = actorLabel.str();
            }
            copied.hasItem = item.has_value();
            copied.handEmpty = readItems && snapshot.handEmpty;
            copied.hasGameType = snapshot.hasGameType;
            copied.spectator = snapshot.spectator;
            if (item) {
                copied.identifier = item->identifier;
                copied.displayName = item->displayName;
                copied.count = item->count;
            }
            // Hive can return to its lobby inside the same Level and player.
            // Reset once on that boundary, retaining roles during ordinary swaps.
            if (config->murderDetect && murderRoundHasRole &&
                tablist::isMurderLobbyItem(copied.identifier)) {
                recordLevel(level, true);
                copied.worldEpoch = murderWorldEpoch.load(std::memory_order_acquire);
                enqueueMurderSample(std::move(copied));
                return; // recordLevel invalidated the Actor roster.
            }
            if (config->murderDetect && copied.hasItem && tablist::murderRoleForHeldItem(
                    copied.identifier, copied.displayName) != tablist::MurderRole::None)
                murderRoundHasRole = true;
            enqueueMurderSample(std::move(copied));
        }
        static std::int64_t nextScanLog=0;
        const auto now=steadyMilliseconds();
        if(now>=nextScanLog) {
            nextScanLog=now+5000;
            diagnostic("MurderDetect equipment sweep: tracked=",murderScanActors.size(),
                " target_ms=",config->murderScanIntervalMs," planned_per_tick=",sampleCount,
                " attempted=",attempted," budget_limited=",attempted<sampleCount,
                " native_us=",std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count(),
                " [750us cooperative budget; maximum 12 Actors/tick; copied snapshots to worker]");
        }
    } catch (...) {
        // Item inspection is observational; never interfere with the game tick.
    }
}

void refreshTrackedMurderActors(void* level) noexcept {
    if (!level) return;
    try {
        if (murderScanLevel != level) {
            murderScanLevel = level;
            murderScanActors.clear();
            murderKnownPlayerNamesByRuntimeId.clear();
            murderKnownPlayerNamesByActor.clear();
            pendingActorRoster.reset();
            nextActorRosterMs = 0;
            lastMurderRosterStatusLogMs = 0;
            murderScanCursor = 0;
            murderLastProcessedScanKey = 0;
        }
        if (!pendingActorRoster) {
            if (steadyMilliseconds() < nextActorRosterMs) return;
            ActorRosterBuild build;
            build.handles = tabListGameAccess.actorHandles(level);
            // Sorting/deduplication touches opaque handles only; native validation is deferred.
            std::sort(build.handles.begin(), build.handles.end(), std::less<void*>{});
            build.handles.erase(std::unique(build.handles.begin(), build.handles.end()),
                                build.handles.end());
            build.tracked.reserve(std::min(build.handles.size(), murderPlayerNames.size()));
            pendingActorRoster = std::move(build);
            nextActorRosterMs = steadyMilliseconds() + 2000;
            return; // Do not combine native enumeration and Actor reads on one tick.
        }
        auto& build = *pendingActorRoster;
        if (build.cursor < build.handles.size()) {
            auto* actor = build.handles[build.cursor++];
            if (!actor) return;
            std::string nameTag;
            {
                std::unique_lock lock(actorNameTagMutex, std::try_to_lock);
                if (!lock) { --build.cursor; return; }
                const auto tag = actorNameTags.find(actor);
                if (tag != actorNameTags.end()) nameTag = tag->second.nameTag;
            }
            auto playerName = matchPlayerNameFromTag(nameTag, murderPlayerNames);
            // A handle with no PlayerList match needs an ID read only if a
            // prior player ID exists that it could reattach to.
            const auto knownPointer = murderKnownPlayerNamesByActor.find(actor);
            if (playerName.empty() && knownPointer == murderKnownPlayerNamesByActor.end() &&
                murderKnownPlayerNamesByRuntimeId.empty()) return;
            // This reader has its own SEH boundary; one native ID read per step.
            const auto runtimeId = readActorRuntimeId(actor);
            if (runtimeId && !build.runtimeIds.insert(*runtimeId).second) return;
            if (playerName.empty() && runtimeId) {
                const auto known = murderKnownPlayerNamesByRuntimeId.find(*runtimeId);
                if (known != murderKnownPlayerNamesByRuntimeId.end()) playerName = known->second;
            }
            if (playerName.empty() && !runtimeId) {
                const auto known = murderKnownPlayerNamesByActor.find(actor);
                if (known != murderKnownPlayerNamesByActor.end()) playerName = known->second;
            }
            if (std::find(murderPlayerNames.begin(), murderPlayerNames.end(), playerName) ==
                murderPlayerNames.end()) return;
            const auto scanKey = runtimeId ? static_cast<std::uintptr_t>(*runtimeId)
                : reinterpret_cast<std::uintptr_t>(actor);
            build.tracked.push_back({actor, runtimeId.value_or(0), scanKey, playerName});
            const auto active = std::find_if(murderScanActors.begin(), murderScanActors.end(),
                [&](const auto& entry) { return entry.scanKey == scanKey; });
            if (active == murderScanActors.end()) murderScanActors.push_back(build.tracked.back());
            else *active = build.tracked.back();
            if (runtimeId) build.namesById.insert_or_assign(*runtimeId, std::move(playerName));
            else build.namesByActor.insert_or_assign(actor, std::move(playerName));
            return;
        }
        std::sort(build.tracked.begin(), build.tracked.end(),
            [](const auto& a, const auto& b) { return a.scanKey < b.scanKey; });
        murderScanActors = std::move(build.tracked);
        murderKnownPlayerNamesByRuntimeId = std::move(build.namesById);
        murderKnownPlayerNamesByActor = std::move(build.namesByActor);
        const auto next = std::upper_bound(murderScanActors.begin(), murderScanActors.end(),
            murderLastProcessedScanKey,
            [](std::uintptr_t key, const MurderActorEntry& actor) { return key < actor.scanKey; });
        murderScanCursor = next == murderScanActors.end() ? 0 :
            static_cast<std::size_t>(next - murderScanActors.begin());
        const auto now = steadyMilliseconds();
        if (now - lastMurderRosterStatusLogMs >= 10000 || lastMurderRosterStatusLogMs == 0) {
            diagnostic("MurderDetect roster: actors=", build.handles.size(),
                       " trackedPlayers=", murderScanActors.size(),
                       " players=", murderPlayerNames.size(), " [incremental RuntimeID reads]");
            lastMurderRosterStatusLogMs = now;
        }
        pendingActorRoster.reset();
        nextActorRosterMs = steadyMilliseconds() + 2000;
    } catch (...) {
        pendingActorRoster.reset();
        nextActorRosterMs = steadyMilliseconds() + 2000;
    }
}

void onLevelTick(const bedrock::LevelTickEvent& event) {
    recordLevel(event.level);
    const auto config = liveConfig.load(std::memory_order_acquire);
    if (!config->enabled) return;
    tabListGameAccess.setDisplayNamesEnabled(config->showHeldItems);
    static tablist::NativeUpdateScheduler scheduler;
    static std::uint64_t epoch = 0;
    const auto currentEpoch = murderWorldEpoch.load(std::memory_order_acquire);
    if (epoch != currentEpoch) {
        epoch = currentEpoch;
        scheduler.reset();
        pendingActorRoster.reset();
        murderScanLevel = nullptr;
        murderScanActors.clear();
        murderPlayerNames.clear();
    }
    const auto now = steadyMilliseconds();
    const bool readItems = config->murderDetect || config->showHeldItems;
    const bool detect = readItems || config->gamemodeCheck;
    if (!config->murderDetect) murderRoundHasRole = false;
    const auto rows = cachedTabRows.load(std::memory_order_acquire);
    static std::array<std::int64_t, 4> maximumUs{};
    static std::int64_t nextReport = 0;
    const auto start = std::chrono::steady_clock::now();
    if (detect) updateMurderDetection(readItems, config->gamemodeCheck);
    const auto itemUs=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
    maximumUs[0]=std::max(maximumUs[0],static_cast<std::int64_t>(itemUs));
    // Maintenance keeps a guaranteed turn at least every three callbacks even
    // if a single native equipment getter exceeds the cooperative time budget.
    static unsigned maintenanceTurn=0;
    const bool maintenanceDue=(++maintenanceTurn%3)==0;
    if(!maintenanceDue && std::chrono::steady_clock::now()-start>=std::chrono::microseconds(750)) return;
    const auto work = scheduler.choose(now, {
        false, // Equipment is distributed on every tick, independently of maintenance.
        tabRowsLastStartedMs == 0 || now - tabRowsLastStartedMs >= 1000,
        detect && !murderPlayerNames.empty() &&
            (pendingActorRoster.has_value() || now >= nextActorRosterMs),
        config->showFaces && now >= nextFaceReadMs && rows &&
            std::any_of(rows->begin(), rows->end(), [](const auto& row) { return !row.hasFace; })
    });
    if (!work) return;
    const auto stageStart=std::chrono::steady_clock::now();
    try {
        switch (*work) {
        case tablist::NativeUpdate::Item:
            break; // Equipment runs above; never schedule another sweep here.
        case tablist::NativeUpdate::Players:
            refreshTabRowsCache(config);
            break;
        case tablist::NativeUpdate::Roster:
            refreshTrackedMurderActors(event.level);
            break;
        case tablist::NativeUpdate::Face:
            refreshOneCachedFace(config);
            break;
        }
    } catch (...) {
        // Allocation/cache failure must not escape the native update callback.
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - stageStart).count();
    const auto index = static_cast<std::size_t>(*work);
    maximumUs[index] = std::max(maximumUs[index], static_cast<std::int64_t>(elapsed));
    if ((config->debug || elapsed >= 2000 || maximumUs[0]>=2000) && now >= nextReport) {
        nextReport = now + 10000;
        diagnostic("TabList native max us: item=", maximumUs[0], " players=", maximumUs[1],
                   " roster=", maximumUs[2], " face=", maximumUs[3],
                   " [bounded item slice + at most one maintenance stage per callback]");
        maximumUs.fill(0);
    }
}

void drawTabList(void*) {
    const auto config = liveConfig.load(std::memory_order_acquire);
    if (!config->enabled || !tabHeld.load() || !gameplayActive.load() || !levelIsFresh()) return;
    const auto players = cachedTabRows.load(std::memory_order_acquire);
    tab_overlay::paintTabList(*config, players ? std::span<const PlayerRow>{*players}
                                             : std::span<const PlayerRow>{}, packetRosterMode);
}

void stopSdkSession() noexcept {
    if(gameHooks) gameHooks->stop();
    packets.stop();
    packetPlayers.stop(packets);
    stopTabRowsWorker(); stopMurderWorker();
    // Retain typed API/callback owners until process exit, including in-flight calls.
    gameplayActive=false; tabHeld=false; levelInstance=nullptr; levelHeartbeatMs=0;
}

} // namespace

void onTabListKeybind(bool pressed) {
    const bool previous = tabHeld.exchange(pressed, std::memory_order_acq_rel);
    if (previous != pressed) diagnostic("tab ", pressed ? 1 : 0);
}

void setTabListConfig(const tablist::Config& config) {
    liveConfig.store(std::make_shared<const tablist::Config>(config), std::memory_order_release);
}

void startTabList(const bedrock::RuntimeProfile& profile, const std::filesystem::path& directory) {
    std::string error;
    if (!diagnosticWriter.start(directory / L"DynamicClient-TabList.log"))
        throw std::runtime_error("could not start TabList diagnostics");
    struct OnFailure {
        bool committed{};
        ~OnFailure() { if (!committed) { stopSdkSession(); diagnosticWriter.stop(); } }
    } startup;
    diagnostic("TabList roster revision=6 [SDK NativeGameAccess / typed GameHooks]");
    const bedrock::ProfileRequirements requirements{
        bedrock::Feature::Input,bedrock::Feature::Gameplay,bedrock::Feature::Fov,
        bedrock::Feature::LevelTick,bedrock::Feature::Actors,bedrock::Feature::Items};
    if(!profile.supports(requirements,error) || !tabListGameAccess.load(profile,error)) {
        throw std::runtime_error(error);
    }
    const auto& version=profile.choice();
    diagnostic("SDK profile ready version=",version.version," source=",bedrock::profileSourceName(version.source),
        " exe_sha256=",version.identity.fileSha256);
    packetRosterMode=tabListGameAccess.packetRosterSupported();
    diagnostic("SDK item display names=",tabListGameAccess.displayNamesAvailable()," [owned value copy / game allocator]");
    tabListGameAccess.setRosterProvider([] { return packetPlayers.snapshotShared(); });
    tabListGameAccess.setNameProvider([](void* actor)->std::optional<std::string> {
        std::unique_lock lock(actorNameTagMutex,std::try_to_lock);
        if(!lock) return std::nullopt;
        const auto found=actorNameTags.find(actor);
        return found==actorNameTags.end()?std::nullopt:std::optional<std::string>{found->second.nameTag};
    });
    eventDispatch=std::make_unique<bedrock::detail::HookDispatch>(tabListGameAccess);
    gameHooks=std::make_unique<bedrock::GameHooks>(*eventDispatch);
    eventDispatch->on<bedrock::GameplayEvent>(&onGameplay);
    eventDispatch->on<bedrock::FovEvent>([](const bedrock::FovEvent&) { gameplayActive=true; });
    eventDispatch->on<bedrock::LevelTickEvent>(&onLevelTick);
    eventDispatch->on<bedrock::ActorNameTagEvent>([](const bedrock::ActorNameTagEvent& event) {
        captureQueriedActorNameTag(event.actor,std::string(event.nameTag));
    });
    if(!startTabRowsWorker() || !startMurderWorker()) {
        throw std::runtime_error("SDK snapshot workers unavailable");
    }
    if(packetRosterMode) {
        packetPlayers.start(packets);
        if(!packets.installReceive(profile.json(),error)) {
            throw std::runtime_error(error);
        }
        diagnostic("SDK packet roster ready; rejoin after loading");
    }
    if(!gameHooks->installInput(profile.resolver(),error) ||
       !gameHooks->installGameplay(profile.resolver(),error) ||
       !gameHooks->installFov(profile.resolver(),error) ||
       !gameHooks->installLevelTick(profile.resolver(),error)) {
        throw std::runtime_error(error);
    }
    if(!gameHooks->installActorNameTag(profile.resolver(),error))
        diagnostic("SDK name-tag source unavailable: ",error);
    else diagnostic("SDK typed name-tag source ready");

    bedrock::gui::OverlayOptions options;
    options.toggleVirtualKey = 0;
    options.initiallyVisible = true;
    options.captureInput = false;
    options.nativeCursor = false;
    diagnostic("overlay install");
    if (!bedrock::gui::Overlay::install(&drawTabList, nullptr, error, options)) {
        throw std::runtime_error(error);
    }

    diagnostic("ready ", version.version, " ", bedrock::profileSourceName(version.source));
    startup.committed = true;
}
} // namespace dynamic_client
