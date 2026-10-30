#pragma once

namespace bedrock::detail {
// Limit the experimental session override to the synchronous broadcast which
// initiated it, the matching game, and the two checked native call sites.
class ScopedSuspendGate final {
  public:
    explicit ScopedSuspendGate(void *game) noexcept : previous_(current_) { current_ = game; }
    ~ScopedSuspendGate() { current_ = previous_; }
    ScopedSuspendGate(const ScopedSuspendGate &) = delete;
    ScopedSuspendGate &operator=(const ScopedSuspendGate &) = delete;

    static bool matches(bool enabled, const void *game, const void *caller,
                        const void *first, const void *second) noexcept {
        return enabled && game && game == current_ && caller &&
               (caller == first || caller == second);
    }

  private:
    inline static thread_local void *current_{};
    void *previous_{};
};
} // namespace bedrock::detail
