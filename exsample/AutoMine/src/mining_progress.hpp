#pragma once
#include <optional>

namespace automine {
// Lifetime is a target/stance trial, not a native destroy session or route.
template <class Cell> class MiningProgress {
public:
    static constexpr unsigned stallTicks = 60;
    bool track(Cell target, unsigned long long tick) {
        if (!target_ || *target_ != target) {
            clear();
            target_ = target;
            advanceTick_ = tick;
            return true;
        }
        if (pausedAt_) {
            advanceTick_ += tick - *pausedAt_;
            pausedAt_.reset();
        }
        return false;
    }
    bool observe(Cell target, unsigned long long tick, float progress) {
        if (target_ && *target_ == target && !pausedAt_ && progress > peak_ + .0001f) {
            peak_ = progress;
            advanceTick_ = tick;
            return true;
        }
        return false;
    }
    bool stalled(unsigned long long tick) const {
        return target_ && !pausedAt_ && tick - advanceTick_ >= stallTicks;
    }
    void pause(unsigned long long tick) {
        if (target_ && !pausedAt_)
            pausedAt_ = tick;
    }
    void clear() { *this = {}; }
    float peak() const { return peak_; }

private:
    std::optional<Cell> target_;
    std::optional<unsigned long long> pausedAt_;
    float peak_{};
    unsigned long long advanceTick_{};
};
} // namespace automine
