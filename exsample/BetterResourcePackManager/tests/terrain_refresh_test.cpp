#include <bedrock/detail/pending_terrain_refresh.hpp>
#include <bedrock/detail/scoped_suspend_gate.hpp>
#include <bedrock/detail/resource_reload_barrier.hpp>
#include <cassert>
#include <thread>
int main() {
    int game{}, level{}, manager{}, other{};
    bedrock::detail::PendingTerrainRefresh pending;
    bool cancelled{};
    pending.begin(&game, &level, &manager, 42);
    assert(!pending.take(&game, &level, 42, cancelled)); // Atlas not ready.
    assert(!pending.complete(&other));
    assert(pending.complete(&game));
    assert(!pending.complete(&game));
    assert(!pending.take(&game, &level, 42, cancelled, false)); // Geometry/tasks still loading.
    assert(pending.pending());
    assert(!pending.take(&game, &level, 7, cancelled)); // Worker cannot run native refresh.
    assert(!pending.take(&other, &level, 42, cancelled));
    assert(pending.take(&game, &level, 42, cancelled) == &manager);
    assert(!cancelled && !pending.pending());
    assert(!pending.take(&game, &level, 42, cancelled)); // Includes reentrancy.
    pending.begin(&game, &level, &manager, 42);
    pending.complete(&game);
    assert(!pending.take(&game, &other, 42, cancelled)); // World changed/exited.
    assert(cancelled && !pending.pending());
    pending.begin(&game, &level, &manager, 42);
    pending.complete(&game);
    pending.begin(&game, &level, &manager, 42); // Next reload must await its atlas.
    assert(!pending.take(&game, &level, 42, cancelled));
    bedrock::detail::PendingCameraRefresh camera;
    camera.begin(&game, &level, 42);
    assert(camera.game() == &game && camera.thread() == 42);
    assert(!camera.cancelIfChanged(&level));
    assert(camera.cancelIfChanged(&other));
    assert(!camera.game() && camera.thread() == 0);
    camera.begin(&game, &level, 42);
    camera.clear();
    assert(!camera.game());

    // A ready loader alone must not permit refresh during a menu transition.
    bedrock::detail::ResourceReloadBarrier barrier;
    int loader{}, atlas{}, replacement{};
    assert(!barrier.allows(1, &loader, &atlas, true));
    barrier.observe(1, &loader, &atlas, false); // Progress screen still active.
    assert(!barrier.allows(1, &loader, &atlas, true));
    barrier.observe(1, &loader, &atlas, true); // Entire original world frame returned.
    assert(barrier.allows(1, &loader, &atlas, true));
    assert(!barrier.allows(2, &loader, &atlas, true)); // A new request cannot reuse it.
    assert(!barrier.allows(1, &replacement, &atlas, true));
    assert(!barrier.allows(1, &loader, &replacement, true));
    assert(!barrier.allows(1, &loader, &atlas, false));
    barrier.observe(1, &loader, &atlas, false); // Work resumed; old idle frame invalid.
    assert(!barrier.allows(1, &loader, &atlas, true));
    barrier.observe(1, &loader, nullptr, true);
    assert(!barrier.allows(1, &loader, nullptr, true));
    assert(bedrock::detail::isWorldRenderScreen("hud_screen"));
    assert(bedrock::detail::isWorldRenderScreen("pause_screen"));
    assert(!bedrock::detail::isWorldRenderScreen("progress_popup"));
    assert(!bedrock::detail::isWorldRenderScreen("progress_screen"));
    assert(!bedrock::detail::isWorldRenderScreen("screen"));
    assert(!bedrock::detail::isWorldRenderScreen("screen_world_controls_and_settings - global_texture_pack_tab"));
    const auto generation = pending.generation();
    pending.begin(&game, &level, &manager, 42);
    assert(pending.generation() != generation && !pending.atlasReady());

    using Gate = bedrock::detail::ScopedSuspendGate;
    int call1{}, call2{}, unrelated{};
    assert(!Gate::matches(true, &game, &call1, &call1, &call2));
    {
        Gate outer(&game);
        assert(Gate::matches(true, &game, &call1, &call1, &call2));
        assert(Gate::matches(true, &game, &call2, &call1, &call2));
        assert(!Gate::matches(false, &game, &call1, &call1, &call2));
        assert(!Gate::matches(true, &other, &call1, &call1, &call2));
        assert(!Gate::matches(true, &game, &unrelated, &call1, &call2));
        std::thread otherThread([&] { assert(!Gate::matches(true, &game, &call1, &call1, &call2)); });
        otherThread.join();
        { Gate inner(&other); assert(!Gate::matches(true, &game, &call1, &call1, &call2)); }
        assert(Gate::matches(true, &game, &call1, &call1, &call2));
    }
    assert(!Gate::matches(true, &game, &call1, &call1, &call2));
}
