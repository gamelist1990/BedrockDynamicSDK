#include <bedrock/sdk.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>

namespace {

constexpr std::uint64_t ms(std::uint64_t value) {
    return value * 1'000'000ull;
}

void testFrameCounter() {
    bedrock::sdk::detail::FrameCounter frames;
    // 20 frames spread over the trailing one-second window.
    for (unsigned i = 0; i < 20; ++i) frames.add(ms(i * 50) + 1);
    assert(frames.fps(ms(999)) == 20.0);
    // The window has moved on: nothing remains.
    assert(frames.fps(ms(2000)) == 0.0);

    frames.add(ms(2000));
    frames.add(ms(2016));
    assert(std::fabs(frames.frameTimeMs() - 16.0) < 1e-9);
}

void testWindowRollover() {
    bedrock::sdk::detail::WindowCounter counter;
    counter.add(ms(0));
    // At t=1000 ms the first frame is exactly one window old and must not
    // count; the new frame occupies the recycled bucket.
    counter.add(ms(1000));
    assert(counter.total(ms(1000)) == 1);
}

void testClickCounter() {
    bedrock::sdk::detail::ClickCounter clicks;
    clicks.press(false, ms(10));
    clicks.press(false, ms(200));
    clicks.press(true, ms(300));
    auto cps = clicks.cps(ms(500));
    assert(cps.left == 2);
    assert(cps.right == 1);

    // A quiet second clears both windows.
    cps = clicks.cps(ms(1400));
    assert(cps.left == 0);
    assert(cps.right == 0);
}

void testPingTracker() {
    bedrock::sdk::detail::PingTracker pings;
    auto empty = pings.snapshot(ms(0));
    assert(empty.samples == 0);
    assert(!empty.connected);

    pings.add(10, ms(0));
    pings.add(30, ms(100));
    pings.add(20, ms(200));
    const auto ping = pings.snapshot(ms(300));
    assert(ping.samples == 3);
    assert(ping.lastMs == 20);
    assert(ping.medianMs == 20);
    assert(ping.connected);

    // No sample for longer than the connected window.
    assert(!pings.snapshot(ms(6000)).connected);
    assert(pings.snapshot(ms(6000)).samples == 3);

    // The ring keeps only the newest samples; the median is over those.
    bedrock::sdk::detail::PingTracker ring;
    for (unsigned value = 1; value <= 40; ++value)
        ring.add(value, ms(value * 10));
    const auto kept = ring.snapshot(ms(400));
    assert(kept.samples == bedrock::sdk::detail::PingTracker::kSamples);
    assert(kept.lastMs == 40);
}

void testUninstalledSnapshot() {
    const auto snapshot = bedrock::sdk::ClientInfo::snapshot();
    assert(!snapshot.fpsAvailable);
    assert(snapshot.fps == 0.0);
    assert(snapshot.ping.samples == 0);
    assert(!snapshot.ping.connected);
    assert(snapshot.cps.left == 0 && snapshot.cps.right == 0);
    assert(!snapshot.device.available);
    assert(snapshot.device.platform.empty());
    assert(snapshot.device.clientVersion.empty());
    assert(bedrock::sdk::ClientInfo::fps() == 0.0);
    assert(bedrock::sdk::ClientInfo::cps().left == 0);
    assert(bedrock::sdk::ClientInfo::device().screenWidth == 0);
}

} // namespace

int main() {
    testFrameCounter();
    testWindowRollover();
    testClickCounter();
    testPingTracker();
    testUninstalledSnapshot();
    return 0;
}
