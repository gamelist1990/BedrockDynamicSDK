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
    void *take(void *game, void *level, std::uint32_t thread, bool &cancelled) noexcept {
        cancelled = false;
        if (!pending() || game != game_ || thread != thread_)
            return nullptr;
        if (level != level_) {
            manager_ = nullptr;
            cancelled = true;
            return nullptr;
        }
        if (!ready_)
            return nullptr;
        auto *result = manager_;
        manager_ = nullptr; // Claim before native code can reenter the getter.
        return result;
    }
};
}
