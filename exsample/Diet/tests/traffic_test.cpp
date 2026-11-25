// Host test for Diet's packet classification and frame/burst correlation.
#include "diet/traffic.hpp"

#include <cassert>
#include <cstdio>
#include <string>

namespace {
constexpr std::uint64_t ms = 1000000ull;

void testSafetyClasses() {
    // Life-critical / inventory / session classes are never deferrable.
    const int critical[] = {0, 2, 3, 5, 19, 18, 25 + 3, 30, 39, 42, 45, 46, 49, 50, 61, 85,
                            113, 147, 148};
    for (const int id : critical) {
        assert(!diet::mayDefer(id));
        assert(diet::classifyPacket(id).safety == diet::SafetyClass::Critical);
    }
    // Unknown ids default to Critical.
    for (const int id : {999, 1023, 7, 57, 58 + 400}) {
        if (id == 58) continue;
        assert(!diet::mayDefer(id));
        assert(diet::classifyPacket(id).safety == diet::SafetyClass::Critical);
    }
    // Heavy delay-tolerant classes.
    for (const int id : {58, 174, 172, 21, 110, 121, 122, 123, 124, 118, 111}) {
        assert(diet::mayDefer(id));
        assert(diet::classifyPacket(id).safety == diet::SafetyClass::DelayTolerant);
    }
    // A negative id can never be deferrable.
    assert(!diet::mayDefer(-1));
    assert(std::string(diet::packetClassName(diet::PacketClass::Chunk)) == "chunk");
    std::puts("safety classes ok");
}

void testFrameSpikes() {
    diet::TrafficAnalyzer analyzer;
    std::uint64_t now = 1000ull * ms;
    // 10 frames at 16 ms, then one 120 ms stall, then 10 more at 16 ms.
    for (int i = 0; i < 11; ++i) {
        now += 16 * ms;
        analyzer.observeFrame(now, 33.0);
    }
    now += 120 * ms;
    analyzer.observeFrame(now, 33.0);
    for (int i = 0; i < 10; ++i) {
        now += 16 * ms;
        analyzer.observeFrame(now, 33.0);
    }
    // Packets arrive in the stall window.
    for (int i = 0; i < 40; ++i) analyzer.observePacket(now - 100 * ms + i * ms, 58, 40000);
    for (int i = 0; i < 3; ++i) analyzer.observePacket(now - 20 * ms + i * ms, 42, 12);

    const auto snapshot = analyzer.snapshot(now + ms, 5000);
    assert(snapshot.frames >= 20);
    assert(snapshot.spikes >= 1);
    assert(snapshot.packets == 43);
    assert(snapshot.frameMsMax >= 120.0);
    assert(snapshot.maxBurstPackets >= 20);
    assert(snapshot.delayTolerantPackets == 40);
    assert(snapshot.delayTolerantBytes == 40ull * 40000);

    const auto report = analyzer.buildReport(now + ms, 5000);
    assert(report.find("traffic window") != std::string::npos);
    assert(report.find("class=chunk") != std::string::npos);
    assert(report.find("defer_tolerant_total") != std::string::npos);
    std::printf("%s", report.c_str());
    std::puts("frame spikes ok");
}

void testWindowExpiry() {
    diet::TrafficAnalyzer analyzer;
    const std::uint64_t now = 100000ull * ms;
    analyzer.observePacket(now, 58, 100);
    analyzer.observeFrame(now, 33.0);
    // 10 s later the old observation must be outside a 5 s window.
    const auto snapshot = analyzer.snapshot(now + 10000 * ms, 5000);
    assert(snapshot.packets == 0);
    assert(snapshot.frames == 0);
    std::puts("window expiry ok");
}

void testLatencyPairing() {
    // Two observations of id 115 inside the gap bound are one round trip.
    {
        diet::TrafficAnalyzer analyzer;
        const std::uint64_t base = 2000ull * ms;
        analyzer.observePacket(base, 115, 9);
        analyzer.observePacket(base + 4 * ms, 115, 9);
        const auto snapshot = analyzer.snapshot(base + 10 * ms);
        assert(snapshot.latencyValid);
        assert(snapshot.latencySamples == 1);
        assert(snapshot.latencyMs > 3.5 && snapshot.latencyMs < 4.5);
        // A lone request with no reply must not produce a measurement.
        analyzer.observePacket(base + 1000 * ms, 115, 9);
        const auto later = analyzer.snapshot(base + 1200 * ms);
        assert(later.latencySamples == 1);
        assert(later.latencyPairs == 1);
    }
    // Steady one-per-second traffic is not a pair and yields no ping.
    {
        diet::TrafficAnalyzer analyzer;
        for (int i = 0; i < 5; ++i)
            analyzer.observePacket(3000ull * ms + static_cast<std::uint64_t>(i) * 1000 * ms, 115, 9);
        const auto snapshot = analyzer.snapshot(8000ull * ms);
        assert(!snapshot.latencyValid);
        assert(snapshot.latencyPairs == 0);
    }
    // Median of several pairs, and other ids never feed the measurement.
    {
        diet::TrafficAnalyzer analyzer;
        const std::uint64_t base = 4000ull * ms;
        analyzer.observePacket(base, 21, 64);
        for (int i = 0; i < 5; ++i) {
            const std::uint64_t at = base + static_cast<std::uint64_t>(i) * 1000 * ms;
            analyzer.observePacket(at, 115, 9);
            analyzer.observePacket(at + static_cast<std::uint64_t>(2 + i) * ms, 115, 9);
        }
        const auto snapshot = analyzer.snapshot(base + 6000 * ms);
        assert(snapshot.latencyValid);
        assert(snapshot.latencySamples == 5);
        assert(snapshot.latencyMs > 3.5 && snapshot.latencyMs < 4.5);
    }
    std::puts("latency pairing ok");
}

void testContentionDropAccounting() {
    diet::TrafficAnalyzer analyzer;
    // Two observations with the same timestamp still land in the ring.
    for (int i = 0; i < 100; ++i) analyzer.observePacket(1000ull * ms + i, 21, 64);
    const auto snapshot = analyzer.snapshot(1000ull * ms + 100, 5000);
    assert(snapshot.packets == 100);
    assert(analyzer.droppedSamples() == 0);
    std::puts("ring ok");
}
} // namespace

int main() {
    testSafetyClasses();
    testFrameSpikes();
    testWindowExpiry();
    testLatencyPairing();
    testContentionDropAccounting();
    std::puts("diet_traffic_test passed");
    return 0;
}
