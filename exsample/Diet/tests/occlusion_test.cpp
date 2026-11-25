// Host test for Diet's occlusion decision core.
#include "diet/occlusion.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
std::uint64_t gClockNs = 0;
std::uint64_t fakeClock() noexcept { return gClockNs; }

struct World {
    static constexpr int size = 64;
    std::vector<unsigned char> solid = std::vector<unsigned char>(size * size * size, 0);
    unsigned queries{};
    void set(int x, int y, int z, bool value) {
        if (x < 0 || y < 0 || z < 0 || x >= size || y >= size || z >= size) return;
        solid[(z * size + y) * size + x] = value ? 1 : 0;
    }
    bool get(int x, int y, int z) const {
        if (x < 0 || y < 0 || z < 0 || x >= size || y >= size || z >= size) return false;
        return solid[(z * size + y) * size + x] != 0;
    }
};

bool probe(void* context, int x, int y, int z) {
    auto* world = static_cast<World*>(context);
    ++world->queries;
    return world->get(x, y, z);
}

void wall(World& world, int x) {
    for (int y = 0; y < 8; ++y)
        for (int z = 0; z < 8; ++z) world.set(x, y, z, true);
}

void testTrace() {
    World world;
    const diet::Vec3f from{0.5f, 0.5f, 0.5f};
    const diet::Vec3f to{20.5f, 0.5f, 0.5f};
    assert(!diet::traceBlocked(&probe, &world, from, to, 256));
    wall(world, 10);
    assert(diet::traceBlocked(&probe, &world, from, to, 256));
    // The target's own voxel is never tested: a target standing inside the wall
    // is not occluded by the cell it occupies.
    const diet::Vec3f insideWall{10.5f, 0.5f, 0.5f};
    assert(!diet::traceBlocked(&probe, &world, from, insideWall, 256));
    // Zero-length and non-finite rays are rejected rather than guessed.
    assert(!diet::traceBlocked(&probe, &world, from, from, 256));
    const diet::Vec3f nanPoint{std::nanf(""), 0.5f, 0.5f};
    assert(!diet::traceBlocked(&probe, &world, from, nanPoint, 256));
    std::puts("trace ok");
}

void testSamples() {
    diet::Aabb box{};
    box.min = {10.0f, 0.0f, 10.0f};
    box.max = {11.0f, 2.0f, 11.0f};
    const diet::Vec3f camera{0.0f, 0.0f, 0.0f};
    const auto plan = diet::planSamples(box, camera, 3);
    assert(plan.count == 3);
    const auto square = [&camera](const diet::Vec3f& p) {
        const float dx = p.x - camera.x, dy = p.y - camera.y, dz = p.z - camera.z;
        return dx * dx + dy * dy + dz * dz;
    };
    assert(square(plan.points[0]) <= square(plan.points[1]) + 1e-3f);
    for (unsigned i = 0; i < plan.count; ++i) {
        assert(plan.points[i].x >= box.min.x - 0.01f && plan.points[i].x <= box.max.x + 0.01f);
        assert(plan.points[i].y >= box.min.y - 0.01f && plan.points[i].y <= box.max.y + 0.01f);
    }
    // Degenerate box still yields one sample.
    diet::Aabb point{};
    point.min = point.max = {5.0f, 5.0f, 5.0f};
    assert(diet::planSamples(point, camera, 3).count >= 1);
    // Invalid box yields none.
    diet::Aabb invalid{};
    invalid.min = {1.0f, 1.0f, 1.0f};
    invalid.max = {0.0f, 0.0f, 0.0f};
    assert(diet::planSamples(invalid, camera, 3).count == 0);
    std::puts("samples ok");
}

diet::Aabb boxAt(float x, float y, float z) {
    diet::Aabb box{};
    box.min = {x - 0.3f, y, z - 0.3f};
    box.max = {x + 0.3f, y + 1.8f, z + 0.3f};
    return box;
}

diet::Limits relaxed() {
    diet::Limits limits;
    limits.nearGuard = 0.0f;
    limits.hideAfter = 2;
    limits.maxSamples = 1;
    limits.maxDistance = 100.0f;
    limits.maxChecksPerTick = 8;
    limits.tickBudgetUs = 1000000; // no budget pressure in tests
    limits.retainMs = 2000;
    return limits;
}

void tick(diet::OcclusionEngine& engine, std::uint64_t now, const diet::Vec3f& camera,
          World& world) {
    gClockNs = now;
    engine.beginTick(now, camera, &probe, &world);
}
void end(diet::OcclusionEngine& engine, std::uint64_t now) {
    gClockNs = now;
    engine.endTick(now);
}

void testHysteresis() {
    World world;
    wall(world, 10);
    diet::OcclusionEngine engine;
    engine.setLimits(relaxed());
    engine.setClock(&fakeClock);
    const diet::Vec3f camera{0.5f, 1.0f, 0.5f};
    const diet::TargetInput target{42, boxAt(20.0f, 0.0f, 0.5f), false};

    std::uint64_t now = 1000000000ull;
    for (int t = 0; t < 4; ++t) {
        tick(engine, now, camera, world);
        engine.addTarget(target);
        end(engine, now);
        const auto& verdict = engine.verdicts()[0];
        if (t == 0) assert(!verdict.hidden); // one occluded check is not enough
        if (t >= 1) assert(verdict.hidden);
        assert(verdict.kind == diet::VerdictKind::Occluded);
        now += 200ull * 1000000ull;
    }

    // Any visible sample shows the target immediately.
    world = World{};
    tick(engine, now, camera, world);
    engine.addTarget(target);
    end(engine, now);
    assert(!engine.verdicts()[0].hidden);
    assert(engine.verdicts()[0].streak == 0);
    std::puts("hysteresis ok");
}

void testDistanceStage() {
    World world;
    diet::OcclusionEngine engine;
    auto limits = relaxed();
    limits.maxDistance = 64.0f;
    engine.setLimits(limits);
    engine.setClock(&fakeClock);
    const diet::Vec3f camera{0.5f, 1.0f, 0.5f};
    const diet::TargetInput far{7, boxAt(200.0f, 0.0f, 0.5f), false};
    world.queries = 0;
    const std::uint64_t now = 5000000000ull;
    tick(engine, now, camera, world);
    engine.addTarget(far);
    end(engine, now);
    assert(engine.verdicts()[0].hidden);
    assert(engine.verdicts()[0].kind == diet::VerdictKind::DistanceCulled);
    assert(world.queries == 0); // no raycast at all beyond the distance stage
    assert(engine.stats().distanceCulled == 1);
    std::puts("distance ok");
}

void testPlayerAndNearGuard() {
    World world;
    wall(world, 2);
    diet::OcclusionEngine engine;
    auto limits = relaxed();
    limits.nearGuard = 3.0f;
    engine.setLimits(limits);
    engine.setClock(&fakeClock);
    const diet::Vec3f camera{0.5f, 1.0f, 0.5f};
    const diet::TargetInput player{11, boxAt(20.0f, 0.0f, 0.5f), true};
    const diet::TargetInput near{12, boxAt(1.0f, 0.0f, 0.5f), false};
    std::uint64_t now = 1000000000ull;
    for (int t = 0; t < 5; ++t) {
        tick(engine, now, camera, world);
        engine.addTarget(player);
        engine.addTarget(near);
        end(engine, now);
        for (unsigned i = 0; i < engine.verdictCount(); ++i)
            assert(!engine.verdicts()[i].hidden);
        now += 200ull * 1000000ull;
    }
    assert(engine.stats().protectedTargets > 0);
    std::puts("player/near guard ok");
}

void testBudget() {
    World world;
    diet::OcclusionEngine engine;
    auto limits = relaxed();
    limits.tickBudgetUs = 0;
    limits.maxChecksPerTick = 32;
    engine.setLimits(limits);
    engine.setClock(&fakeClock);
    const diet::Vec3f camera{0.5f, 1.0f, 0.5f};
    const std::uint64_t now = 9000000000ull;
    gClockNs = now + 1000; // already past the zero-length budget
    engine.beginTick(now, camera, &probe, &world);
    for (int i = 0; i < 8; ++i)
        engine.addTarget({static_cast<std::uint64_t>(100 + i), boxAt(20.0f + i, 0.0f, 0.5f), false});
    engine.endTick(now);
    assert(engine.stats().checked == 0); // an exhausted budget never runs a check
    assert(engine.stats().hidden == 0);  // and never hides anything
    assert(engine.verdictCount() == 8);
    std::puts("budget ok");
}

void testRetentionAndChecksPerTick() {
    World world;
    diet::OcclusionEngine engine;
    auto limits = relaxed();
    limits.maxChecksPerTick = 1;
    limits.retainMs = 500;
    engine.setLimits(limits);
    engine.setClock(&fakeClock);
    const diet::Vec3f camera{0.5f, 1.0f, 0.5f};
    std::uint64_t now = 1000000000ull;
    tick(engine, now, camera, world);
    for (int i = 0; i < 4; ++i)
        engine.addTarget({static_cast<std::uint64_t>(200 + i), boxAt(20.0f + i, 0.0f, 0.5f), false});
    end(engine, now);
    assert(engine.stats().checked <= 1); // one check per tick regardless of target count
    assert(engine.verdictCount() == 4);

    // Nobody is offered for longer than the retention window: everything expires.
    now += 600ull * 1000000ull;
    tick(engine, now, camera, world);
    end(engine, now);
    assert(engine.verdictCount() == 0);
    std::puts("retention ok");
}
} // namespace

int main() {
    testTrace();
    testSamples();
    testHysteresis();
    testDistanceStage();
    testPlayerAndNearGuard();
    testBudget();
    testRetentionAndChecksPerTick();
    std::puts("diet_occlusion_test passed");
    return 0;
}
