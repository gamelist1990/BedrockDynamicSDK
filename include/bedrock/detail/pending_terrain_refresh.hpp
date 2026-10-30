#pragma once
#include <cstdint>

namespace bedrock::detail {
// Synchronization belongs to the adapter. Object addresses are identity tokens;
// this state never dereferences a game, level, manager, or retained client.
class PendingTerrainRefresh {
    void *game_{}, *level_{}, *manager_{};
    std::uint32_t thread_{};
    bool ready_{}, claimed_{};
    std::uint64_t generation_{};
  public:
    void begin(void *game, void *level, void *manager, std::uint32_t thread) noexcept {
        ++generation_;
        game_ = game;
        level_ = level;
        manager_ = manager;
        thread_ = thread;
        ready_ = claimed_ = false;
    }
    bool complete(void *game, std::uint64_t generation) noexcept {
        if (!pending() || game != game_ || generation != generation_ || ready_)
            return false;
        ready_ = true;
        return true;
    }
    // A stale completion/claim must never consume or requeue a newer selection.
    void finish(std::uint64_t generation, bool retry = false) noexcept {
        if (generation != generation_ || !claimed_) return;
        claimed_ = false;
        if (!retry) manager_ = nullptr;
    }
    std::uint64_t generation() const noexcept { return generation_; }
    bool atlasReady() const noexcept { return ready_; }
    bool pending() const noexcept { return manager_ != nullptr; }
    void *take(void *game, void *level, std::uint32_t thread, bool &cancelled, bool resourcesReady = true) noexcept {
        cancelled = false;
        if (!pending() || game != game_ || thread != thread_)
            return nullptr;
        if (level != level_) {
            manager_ = nullptr;
            cancelled = true;
            return nullptr;
        }
        if (claimed_ || !ready_ || !resourcesReady)
            return nullptr;
        auto *result = manager_;
        claimed_ = true; // Keep the request until native refresh actually succeeds.
        return result;
    }
};
// Identity only: no retained ownership. Calls are synchronized by the adapter.
class PendingCameraRefresh {
    void *game_{}, *level_{};
    std::uint32_t thread_{};
  public:
    void begin(void *game, void *level, std::uint32_t thread) noexcept { game_=game; level_=level; thread_=thread; }
    void *game() const noexcept { return game_; }
    std::uint32_t thread() const noexcept { return thread_; }
    void clear() noexcept { game_=nullptr; level_=nullptr; thread_=0; }
    bool cancelIfChanged(void *level) noexcept {
        if (game_ && level != level_) { clear(); return true; }
        return false;
    }
};
}
