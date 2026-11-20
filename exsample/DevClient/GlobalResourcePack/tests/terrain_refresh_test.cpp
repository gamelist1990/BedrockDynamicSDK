#include <bedrock/detail/pending_terrain_refresh.hpp>
#include <bedrock/detail/resource_reload_barrier.hpp>
#include <cassert>
int main() {
    int game{}, level{}, manager{}, other{};
    bedrock::detail::PendingTerrainRefresh pending;
    bool cancelled{};
    pending.begin(&game, &level, &manager, 42);
    assert(!pending.take(&game, &level, 42, cancelled)); // Atlas not ready.
    assert(!pending.complete(&other, pending.generation()));
    assert(pending.complete(&game, pending.generation()));
    assert(!pending.complete(&game, pending.generation()));
    assert(!pending.take(&game, &level, 42, cancelled, false)); // Geometry/tasks still loading.
    assert(pending.pending());
    assert(!pending.take(&game, &level, 7, cancelled)); // Worker cannot run native refresh.
    assert(!pending.take(&other, &level, 42, cancelled));
    assert(pending.take(&game, &level, 42, cancelled) == &manager);
    assert(!cancelled && pending.pending()); // Claim is retained until success.
    assert(!pending.take(&game, &level, 42, cancelled)); // Includes reentrancy.
    pending.finish(pending.generation());
    assert(!pending.pending());
    assert(!pending.take(&game, &level, 42, cancelled)); // Includes reentrancy.
    pending.begin(&game, &level, &manager, 42);
    pending.complete(&game, pending.generation());
    assert(!pending.take(&game, &other, 42, cancelled)); // World changed/exited.
    assert(cancelled && !pending.pending());
    pending.begin(&game, &level, &manager, 42);
    pending.complete(&game, pending.generation());
    pending.begin(&game, &level, &manager, 42); // Next reload must await its atlas.
    assert(!pending.take(&game, &level, 42, cancelled));
    // Additional work discovered after draining must not drop a selection.
    assert(pending.complete(&game, pending.generation()));
    const auto retryGeneration = pending.generation();
    assert(pending.take(&game, &level, 42, cancelled) == &manager);
    pending.finish(retryGeneration, true);
    assert(pending.pending());
    assert(!pending.take(&game, &level, 42, cancelled, false));
    assert(pending.take(&game, &level, 42, cancelled, true) == &manager);
    pending.finish(retryGeneration);
    assert(!pending.pending());

    // Native reentrancy may install a newer request before the old call returns.
    pending.begin(&game, &level, &manager, 42);
    const auto oldGeneration = pending.generation();
    pending.complete(&game, oldGeneration);
    assert(pending.take(&game, &level, 42, cancelled) == &manager);
    pending.begin(&game, &level, &manager, 42);
    assert(!pending.complete(&game, oldGeneration));
    pending.finish(oldGeneration);
    pending.finish(oldGeneration, true);
    assert(pending.pending() && !pending.atlasReady());
    assert(!pending.take(&game, &level, 42, cancelled));
    bedrock::detail::PendingPackApply apply;
    assert(!apply.matches(&game, &level, &manager, 42));
    apply.begin(&game, &level, &manager, 42);
    assert(apply.pending() && apply.matches(&game, &level, &manager, 42));
    // Listener deferral retains eligibility for the later owning frame.
    assert(apply.matches(&game, &level, &manager, 42));
    assert(!apply.matches(&other, &level, &manager, 42));
    assert(!apply.matches(&game, &other, &manager, 42));
    assert(!apply.matches(&game, &level, &other, 42));
    assert(!apply.matches(&game, &level, &manager, 7));
    apply.clear();
    assert(!apply.pending() && !apply.matches(&game, &level, &manager, 42));
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
    barrier.observe(1, &loader, &atlas, false); // Loader still has work.
    assert(!barrier.allows(1, &loader, &atlas, true));
    barrier.observe(1, &loader, &atlas, true); // Entire original frame returned, including while settings stay open.
    assert(barrier.allows(1, &loader, &atlas, true));
    assert(!barrier.allows(2, &loader, &atlas, true)); // A new request cannot reuse it.
    assert(!barrier.allows(1, &replacement, &atlas, true));
    assert(!barrier.allows(1, &loader, &replacement, true));
    assert(!barrier.allows(1, &loader, &atlas, false));
    barrier.observe(1, &loader, &atlas, false); // Work resumed; old idle frame invalid.
    assert(!barrier.allows(1, &loader, &atlas, true));
    barrier.observe(1, &loader, nullptr, true);
    assert(!barrier.allows(1, &loader, nullptr, true));
    const auto generation = pending.generation();
    pending.begin(&game, &level, &manager, 42);
    assert(pending.generation() != generation && !pending.atlasReady());

}
