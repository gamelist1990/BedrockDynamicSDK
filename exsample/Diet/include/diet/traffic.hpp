#pragma once
// Diet — network/frame correlation and packet safety classification.
//
// Host-testable: no Windows, no native game pointers. The Mod feeds frame
// boundaries (render thread) and decoded packet observations (networking
// thread); `buildReport()` runs on an ordinary housekeeping thread and returns
// a human-readable window summary.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace diet {

enum class PacketClass : unsigned char {
    Session = 0,
    LifeCritical,
    Inventory,
    Chunk,
    BlockUpdate,
    EntityMove,
    EntityData,
    Effects,
    World,
    Other,
    Unknown,
    Count,
};

enum class SafetyClass : unsigned char { Critical = 0, DelayTolerant = 1 };

struct PacketClassInfo {
    PacketClass packetClass{PacketClass::Unknown};
    SafetyClass safety{SafetyClass::Critical};
    const char* name{"unknown"};
};

// Protocol id -> (class, safety class). Unknown ids are Critical by rule.
[[nodiscard]] PacketClassInfo classifyPacket(int rawId) noexcept;
[[nodiscard]] const char* packetClassName(PacketClass value) noexcept;
// Only the delay-tolerant classes may ever be deferred. Every unknown id and
// every life-critical/inventory/session id returns false.
[[nodiscard]] bool mayDefer(int rawId) noexcept;

struct TrafficRow {
    PacketClass packetClass{PacketClass::Unknown};
    const char* name{"unknown"};
    std::uint64_t packets{};
    std::uint64_t bytes{};
    std::uint64_t maxBurstPackets{};   // densest 50 ms window for this class
    std::uint64_t delayTolerant{};
};

struct TrafficSnapshot {
    std::uint64_t frames{};
    std::uint64_t packets{};
    std::uint64_t bytes{};
    std::uint64_t spikes{};
    std::uint64_t droppedSamples{};
    double windowSeconds{};
    double fps{};
    double frameMsP50{};
    double frameMsP95{};
    double frameMsMax{};
    double maxBurstMs{};
    std::uint64_t maxBurstPackets{};
    PacketClass maxBurstClass{PacketClass::Unknown};
    std::uint64_t delayTolerantPackets{};
    std::uint64_t delayTolerantBytes{};
};

// Fixed-capacity, lock-guarded observation buffer. Producers never block:
// they try the lock and count a dropped sample instead of waiting on the game
// or network thread.
class TrafficAnalyzer {
public:
    static constexpr std::size_t kPacketRing = 8192;
    static constexpr std::size_t kFrameRing = 512;
    static constexpr std::size_t kSpikeRing = 16;
    static constexpr std::uint64_t kDefaultWindowMs = 5000;

    void observePacket(std::uint64_t nowNs, int rawId, std::size_t bytes) noexcept;
    void observeFrame(std::uint64_t nowNs, double spikeThresholdMs = 33.0) noexcept;

    // Consumes the window [now - windowMs, now]. Not for the render or network
    // thread: it takes both locks and allocates the returned string.
    [[nodiscard]] std::string buildReport(std::uint64_t nowNs,
                                          std::uint64_t windowMs = kDefaultWindowMs);
    [[nodiscard]] TrafficSnapshot snapshot(std::uint64_t nowNs,
                                          std::uint64_t windowMs = kDefaultWindowMs);

    [[nodiscard]] std::uint64_t droppedSamples() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }
    // Duration of the most recent frame in milliseconds. Cheap and lock-free:
    // the tick uses it to stop adding its own work to an already-slow frame.
    [[nodiscard]] double lastFrameMs() const noexcept {
        return static_cast<double>(lastFrameNs_.load(std::memory_order_relaxed)) / 1.0e6;
    }

private:
    struct PacketEntry {
        std::uint64_t ns{};
        std::uint64_t bytes{};
        int rawId{};
    };
    struct FrameEntry {
        std::uint64_t ns{};
        std::uint64_t durationNs{};
    };
    struct SpikeEntry {
        std::uint64_t startNs{};
        std::uint64_t endNs{};
        std::uint64_t durationNs{};
        std::uint32_t packetsInWindow{};
    };

    void collect(std::uint64_t nowNs, std::uint64_t windowMs, TrafficSnapshot& out,
                 TrafficRow* rows, std::size_t& rowCount) noexcept;

    mutable std::mutex packetMutex_;
    std::mutex frameMutex_;
    PacketEntry packets_[kPacketRing]{};
    std::size_t packetNext_{};
    std::uint64_t packetCount_{};
    FrameEntry frames_[kFrameRing]{};
    std::size_t frameNext_{};
    std::uint64_t frameCount_{};
    SpikeEntry spikes_[kSpikeRing]{};
    std::size_t spikeNext_{};
    std::uint64_t spikeCount_{};
    std::uint64_t lastFrameStampNs_{};
    std::atomic<std::uint64_t> lastFrameNs_{};
    std::atomic<std::uint64_t> dropped_{};
};

} // namespace diet
