#pragma once
#include <bedrock/api/actor_samples.hpp>
#include <bedrock/native/actor_components.hpp>
#include <bedrock/native/world_render.hpp>
namespace bedrock {
// Incremental native Player discovery with owning-tick and runtime-ID checks.
// Only copied boxes leave this adapter; the worker never sees registry handles.
class NativePlayerBoundsTracker {
    using Clock = std::chrono::steady_clock;
    using Registry = NativeActorComponents::Registry;

  public:
    void configure(const NativeActorLayout &actors, const WorldRenderProfile &render) {
        actors_ = actors;
        render_ = render;
        trackedCount_ = cursor_ = 0;
        owner_ = 0;
    }
    bool capture(void *actor, ActorBoundsFrame *batch, unsigned limit) noexcept {
        if (!batch)
            return false;
        const auto current = GetCurrentThreadId();
        if (!owner_)
            owner_ = current;
        if (owner_ != current)
            return false;
        const auto &actors = actors_;
        const auto &render = render_;
        // The game thread owns these generation-checked handles. Workers receive
        // only copied AABBs; discovery stays bounded while known Players update each tick.
        auto &tracked = tracked_;
        auto &trackedCount = trackedCount_;
        auto &cursor = cursor_;
        auto &previousRegistry = previousRegistry_;
        auto &previousEpoch = previousEpoch_;
        void *registry{};
        if (!read(actor, actors.actorEntityContextOffset + actors.contextRegistryOffset,
                  registry) ||
            !registry)
            return false;
        if (previousRegistry != registry || previousEpoch != batch->epoch) {
            trackedCount = cursor = 0;
            previousRegistry = registry;
            previousEpoch = batch->epoch;
        }
        batch->made = Clock::now();
        batch->count = 0;
        __try {
            auto *r = static_cast<Registry *>(registry);
            auto *types = r->storage(actors.actorTypeComponentHash);
            auto *owners = r->storage(actors.actorOwnerComponentHash);
            auto *bounds = r->storage(actors.aabbShapeComponentHash);
            auto *ids = r->storage(actors.runtimeIdComponentHash);
            if (!types || !owners || !bounds || !ids)
                return false;
            const auto size = types->size();
            if (size > 16384)
                return false;
            const auto start = Clock::now();
            for (unsigned n = 0; n < limit && n < size; ++n) {
                if (n && Clock::now() - start > std::chrono::microseconds(150))
                    break;
                if (cursor >= size)
                    cursor = 0;
                const auto entity = types->data()[cursor++];
                std::uint32_t type{};
                void *owner{};
                std::uint64_t id{};
                if (!r->valid(entity) || !types->contains(entity) ||
                    !read(types->value(entity), render.actorTypeValueOffset, type) ||
                    type != render.playerTypeId || !owners->contains(entity) ||
                    !read(owners->value(entity), actors.actorOwnerPointerOffset, owner) || !owner ||
                    owner == actor || !ids->contains(entity) ||
                    !read(ids->value(entity), actors.runtimeIdValueOffset, id) || !id)
                    continue;
                std::size_t i = 0;
                for (; i < trackedCount; ++i)
                    if (tracked[i].id == id)
                        break;
                if (i == trackedCount && trackedCount < tracked.size())
                    tracked[trackedCount++] = {entity, id};
            }
            for (std::size_t i = 0; i < trackedCount;) {
                auto &item = tracked[i];
                std::uint64_t id{};
                void *owner{};
                TrackedActorBounds box;
                if (!r->valid(item.entity) || !ids->contains(item.entity) ||
                    !owners->contains(item.entity) ||
                    !read(ids->value(item.entity), actors.runtimeIdValueOffset, id) ||
                    id != item.id ||
                    !read(owners->value(item.entity), actors.actorOwnerPointerOffset, owner) ||
                    !owner || owner == actor || !bounds->contains(item.entity)) {
                    tracked[i] = tracked[--trackedCount];
                    continue;
                }
                auto *shape = bounds->value(item.entity);
                if (!read(shape, actors.aabbMinimumOffset, box.bounds.minimum) ||
                    !read(shape, actors.aabbMaximumOffset, box.bounds.maximum) ||
                    !box.bounds.valid() || !finite(box.bounds.minimum) ||
                    !finite(box.bounds.maximum)) {
                    tracked[i] = tracked[--trackedCount];
                    continue;
                }
                const auto a = box.bounds.minimum, b = box.bounds.maximum;
                if (b.x - a.x > 4 || b.y - a.y > 5 || b.z - a.z > 4) {
                    tracked[i] = tracked[--trackedCount];
                    continue;
                }
                box.id = id;
                box.blendFrom = box.bounds;
                const float dx = a.x - item.bounds.minimum.x, dy = a.y - item.bounds.minimum.y,
                            dz = a.z - item.bounds.minimum.z;
                if (item.seen != Clock::time_point{} &&
                    batch->made - item.seen <= std::chrono::milliseconds(250) &&
                    dx * dx + dy * dy + dz * dz <= 64.f)
                    box.blendFrom = item.bounds;
                item.bounds = box.bounds;
                item.seen = batch->made;
                batch->boxes[batch->count++] = box;
                ++i;
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            batch->count = 0;
            return false;
        }
    }

  private:
    template <class T> static bool read(const void *p, std::size_t offset, T &value) noexcept {
        return native_profile::read(p, offset, value);
    }
    static bool finite(Vec3 value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
               std::abs(value.x) < 40000000 && std::abs(value.y) < 40000000 &&
               std::abs(value.z) < 40000000;
    }
    struct Tracked {
        bedrock::NativeActorComponents::Entity entity{};
        std::uint64_t id{};
        bedrock::ActorBounds bounds{};
        Clock::time_point seen{};
    };
    std::array<Tracked, 128> tracked_{};
    std::size_t trackedCount_ = 0, cursor_ = 0;
    void *previousRegistry_{};
    std::uint64_t previousEpoch_{};

    NativeActorLayout actors_;
    WorldRenderProfile render_;
    DWORD owner_{};
};
} // namespace bedrock
