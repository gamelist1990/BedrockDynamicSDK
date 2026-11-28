#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace bedrock::sdk {

// Clicks per second over the trailing one-second window.
struct CpsSnapshot {
    unsigned left{};   // attack / left button
    unsigned right{};  // use / right button
};

// NetworkStackLatencyPacket round trips observed on this client.
struct PingSnapshot {
    unsigned lastMs{};    // most recent round trip
    unsigned medianMs{};  // rolling median over the retained samples
    unsigned samples{};   // retained samples (0 = never measured)
    bool connected{};     // a sample arrived within the connected window
};

// Static-ish client/session description. screenWidth/screenHeight are the
// client area of the game window in physical pixels (0 when unreadable).
struct DeviceInfo {
    bool available{};
    std::string platform;       // "windows-x64"
    std::string clientVersion;  // selected profile version, e.g. "1.26.65.1"
    unsigned screenWidth{};
    unsigned screenHeight{};
};

struct ClientMetricsSnapshot {
    bool fpsAvailable{};
    double fps{};           // frames in the trailing one-second window
    double frameTimeMs{};   // last observed frame duration
    PingSnapshot ping{};
    CpsSnapshot cps{};
    DeviceInfo device{};
};

namespace detail {

// One-second windows are ten fixed buckets so a read is a bounded scan of
// atomics: no allocation, no locks, no growth with frame rate.
inline constexpr std::uint64_t kMetricsBucketNs = 100'000'000;
inline constexpr std::size_t kMetricsBuckets = 10;

// Producers add with a monotonic timestamp; readers sum the trailing window.
// A bucket rolling over while a reader scans can miss one count for one read.
class WindowCounter {
public:
    void add(std::uint64_t nowNs, unsigned count = 1) noexcept {
        const auto epoch = nowNs / kMetricsBucketNs;
        const auto slot = static_cast<std::size_t>(epoch % kMetricsBuckets);
        if (epochs_[slot].load(std::memory_order_relaxed) != epoch) {
            epochs_[slot].store(epoch, std::memory_order_relaxed);
            counts_[slot].store(0, std::memory_order_relaxed);
        }
        counts_[slot].fetch_add(count, std::memory_order_relaxed);
    }

    unsigned total(std::uint64_t nowNs) const noexcept {
        const auto epoch = nowNs / kMetricsBucketNs;
        unsigned sum = 0;
        for (std::size_t i = 0; i < kMetricsBuckets && i <= epoch; ++i) {
            const auto slot = static_cast<std::size_t>((epoch - i) % kMetricsBuckets);
            if (epochs_[slot].load(std::memory_order_relaxed) == epoch - i)
                sum += counts_[slot].load(std::memory_order_relaxed);
        }
        return sum;
    }

private:
    std::array<std::atomic<std::uint64_t>, kMetricsBuckets> epochs_{};
    std::array<std::atomic<unsigned>, kMetricsBuckets> counts_{};
};

// Frame clock for the render/present path.
class FrameCounter {
public:
    void add(std::uint64_t nowNs) noexcept {
        const auto previous = lastFrameNs_.load(std::memory_order_relaxed);
        if (previous && nowNs > previous)
            lastFrameMs_.store(static_cast<double>(nowNs - previous) / 1.0e6,
                               std::memory_order_relaxed);
        lastFrameNs_.store(nowNs, std::memory_order_relaxed);
        frames_.add(nowNs);
    }

    [[nodiscard]] double fps(std::uint64_t nowNs) const noexcept {
        return static_cast<double>(frames_.total(nowNs));
    }
    [[nodiscard]] double frameTimeMs() const noexcept {
        return lastFrameMs_.load(std::memory_order_relaxed);
    }

private:
    WindowCounter frames_;
    std::atomic<std::uint64_t> lastFrameNs_{};
    std::atomic<double> lastFrameMs_{};
};

// Attack/use click counter for the client input path.
class ClickCounter {
public:
    void press(bool right, std::uint64_t nowNs) noexcept {
        (right ? right_ : left_).add(nowNs);
    }
    [[nodiscard]] CpsSnapshot cps(std::uint64_t nowNs) const noexcept {
        return {left_.total(nowNs), right_.total(nowNs)};
    }

private:
    WindowCounter left_;
    WindowCounter right_;
};

// Fixed-sample latency history. The median is an upper median, matching the
// in-repo measurement (sorted sample at count / 2).
class PingTracker {
public:
    static constexpr std::size_t kSamples = 15;
    static constexpr std::uint64_t kConnectedWindowNs = 5'000'000'000ull;

    void add(unsigned milliseconds, std::uint64_t nowNs) noexcept {
        const auto slot = next_.load(std::memory_order_relaxed) % kSamples;
        values_[slot].store(milliseconds, std::memory_order_relaxed);
        next_.store((slot + 1) % kSamples, std::memory_order_relaxed);
        const auto seen = count_.load(std::memory_order_relaxed);
        if (seen < kSamples) count_.store(seen + 1, std::memory_order_relaxed);
        lastMs_.store(milliseconds, std::memory_order_relaxed);
        lastStampNs_.store(nowNs, std::memory_order_relaxed);
    }

    [[nodiscard]] PingSnapshot snapshot(std::uint64_t nowNs) const noexcept {
        PingSnapshot out;
        const auto seen = std::min(count_.load(std::memory_order_relaxed), kSamples);
        out.samples = static_cast<unsigned>(seen);
        out.lastMs = lastMs_.load(std::memory_order_relaxed);
        std::array<unsigned, kSamples> sorted{};
        for (std::size_t i = 0; i < seen; ++i)
            sorted[i] = values_[i].load(std::memory_order_relaxed);
        std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(seen));
        if (seen) out.medianMs = sorted[seen / 2];
        const auto stamp = lastStampNs_.load(std::memory_order_relaxed);
        out.connected = seen != 0 && nowNs >= stamp && nowNs - stamp <= kConnectedWindowNs;
        return out;
    }

private:
    std::array<std::atomic<unsigned>, kSamples> values_{};
    std::atomic<std::size_t> next_{};
    std::atomic<std::size_t> count_{};
    std::atomic<unsigned> lastMs_{};
    std::atomic<std::uint64_t> lastStampNs_{};
};

} // namespace detail
} // namespace bedrock::sdk
