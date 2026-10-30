#pragma once
#include <cstdint>

namespace bedrock::detail {
// Synchronization belongs to the adapter. Object addresses are identity tokens;
// this state never dereferences a game, level, manager, or retained client.
class PendingTerrainRefresh {
    void *game_{}, *level_{}, *manager_{};
    std::uint32_t thread_{};
    bool ready_{};
  public:
    void begin(void *game, void *level, void *manager, std::uint32_t thread) noexcept {
        game_ = game;
        level_ = level;
        manager_ = manager;
        thread_ = thread;
        ready_ = false;
    }
    bool complete(void *game) noexcept {
        if (!pending() || game != game_ || ready_)
            return false;
        ready_ = true;
        return true;
    }
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
        if (!ready_ || !resourcesReady)
            return nullptr;
        auto *result = manager_;
        manager_ = nullptr; // Claim before native code can reenter the getter.
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
