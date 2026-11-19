#include "../src/mining_progress.hpp"
#include <cassert>

int main() {
    automine::MiningProgress<int> progress;
    constexpr int coal = 1, anotherBlock = 2;
    assert(progress.track(coal, 0));
    for (unsigned tick = 0; tick < 62; ++tick) {
        // Re-entering Mine on the same target and restarting the native
        // attack must not erase the previous .067 high-water mark.
        assert(!progress.track(coal, tick));
        assert(progress.observe(coal, tick, tick % 2 ? .067f : 0.f) == (tick == 1));
        assert(progress.stalled(tick) == (tick >= 61));
    }
    progress.clear(); // only an explicit stance rejection starts a new trial
    assert(progress.track(coal, 100));
    assert(progress.observe(coal, 103, .067f));
    // No observations while the route is being rebuilt; re-enter on the
    // old target without a fresh grace period for the same stance.
    assert(!progress.track(coal, 162));
    assert(!progress.observe(coal, 162, .067f));
    assert(!progress.stalled(162));
    assert(progress.stalled(163));
    progress.clear();
    assert(progress.track(coal, 164));
    for (unsigned tick = 164; tick < 300; ++tick) {
        progress.observe(coal, tick, (tick - 164) * .001f);
        assert(!progress.stalled(tick)); // slow but advancing
    }
    assert(progress.track(anotherBlock, 300));
    assert(!progress.observe(coal, 359, .8f)); // stale progress from the old target
    assert(!progress.stalled(359));
    assert(progress.stalled(360)); // no valid native calls is also bounded
    assert(progress.track(coal, 400));
    assert(progress.observe(coal, 400, .8f));
    progress.pause(420);
    progress.pause(470);
    assert(!progress.track(coal, 500));
    assert(!progress.observe(coal, 539, .1f));
    assert(!progress.stalled(539));
    assert(!progress.observe(coal, 540, .8f));
    assert(progress.stalled(540));
}
