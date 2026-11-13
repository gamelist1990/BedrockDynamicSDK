#pragma once
#include <bedrock/api/actor_samples.hpp>
#include <bedrock/api/geometry.hpp>
#include <bedrock/detail/native_world_access.hpp>
#include <bedrock/native/actors.hpp>
#include <entt/entt.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace bedrock {
// Player-identity knowledge stays with the host: the registry walk only asks
// whether a runtime id is already known to be a player and reports native
// player classifications back.
struct ActorRegistryCapturePolicy {
    bool playersOnly{};
    bool (*knownPlayer)(void *context, std::uint64_t runtimeId) noexcept = nullptr;
    void (*classifyPlayer)(void *context, std::uint64_t runtimeId) noexcept = nullptr;
    void *context = nullptr;
};
// Stage/failure strings mirror the historical in-Mod capture diagnostics so
// existing log readers keep matching.
struct ActorRegistryCaptureStats {
    const char *stage = "registry_context";
    const char *failure = nullptr;
    std::uint32_t faultCode{};
    std::uintptr_t faultAddress{};
    std::size_t population{}, missingRuntime{}, filteredActors{}, invalidOwner{},
        nativePlayers{}, rememberedPlayers{}, missingShape{}, invalidBounds{};
};

// Owning-thread access to the live Actor ECS registry behind the profile's
// NativeActorLayout: component lookup, runtime identity, an incremental
// batch walk and tracked-pose refresh. Every value leaves as a copy in
// ActorRegistrySample; no EnTT iterator, storage or component pointer
// survives any call. All methods are noexcept and guard native reads with
// SEH; a fault mid-walk publishes the rows copied so far.
class NativeActorRegistry {
public:
    void configure(const NativeActorLayout &layout, std::size_t actorTypeValueOffset,
                   std::uint32_t playerTypeId) noexcept {
        layout_ = layout;
        actorTypeValueOffset_ = actorTypeValueOffset;
        playerTypeId_ = playerTypeId;
    }
    const NativeActorLayout &layout() const noexcept { return layout_; }

    void *registryOf(void *actor) const noexcept {
        void *registry = nullptr;
        const auto offset = layout_.actorEntityContextOffset + layout_.contextRegistryOffset;
        return actor && world_native::read(actor, offset, registry) ? registry : nullptr;
    }
    bool readEntityId(void *actor, std::uint32_t &entityId) const noexcept {
        const auto offset = layout_.actorEntityContextOffset + layout_.contextEntityIdOffset;
        return actor && world_native::read(actor, offset, entityId);
    }
    std::uintptr_t registryIdentity(void *actor) const noexcept {
        return reinterpret_cast<std::uintptr_t>(registryOf(actor));
    }
    std::uint64_t runtimeIdentity(void *actor) const noexcept {
        __try {
            void *registry = registryOf(actor);
            std::uint32_t entity = 0;
            if (!registry || !readEntityId(actor, entity))
                return 0;
            using Registry = entt::basic_registry<world_native::Entity>;
            auto *typed = static_cast<Registry *>(registry);
            const world_native::Entity handle{entity};
            auto *storage = typed->storage(layout_.runtimeIdComponentHash);
            std::uint64_t runtime = 0;
            if (!typed->valid(handle) || !storage || !storage->contains(handle) ||
                !world_native::read(storage->value(handle), layout_.runtimeIdValueOffset, runtime))
                return 0;
            return runtime;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }
    void *component(void *actor, std::uint32_t hash, std::uint32_t alternate) const noexcept {
        void *registry = registryOf(actor);
        std::uint32_t rawId = 0;
        if (!registry || !readEntityId(actor, rawId))
            return nullptr;
        __try {
            using Registry = entt::basic_registry<world_native::Entity>;
            auto *typed = static_cast<Registry *>(registry);
            auto *storage = typed->storage(hash);
            const world_native::Entity entity{rawId};
            if ((!storage || !storage->contains(entity)) && alternate)
                storage = typed->storage(alternate);
            if (!storage || !storage->contains(entity))
                return nullptr;
            return storage->value(entity);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }
    bool readBounds(void *actor, ActorBounds &output) const noexcept {
        void *shape = component(actor, layout_.aabbShapeComponentHash,
                                layout_.aabbShapeComponentHashAlternate);
        if (!shape || !world_native::read(shape, layout_.aabbMinimumOffset, output.minimum) ||
            !world_native::read(shape, layout_.aabbMaximumOffset, output.maximum) ||
            !output.valid())
            return false;
        const float width = output.maximum.x - output.minimum.x;
        const float height = output.maximum.y - output.minimum.y;
        const float depth = output.maximum.z - output.minimum.z;
        return width > 0.0f && width <= 64.0f && height > 0.0f && height <= 64.0f &&
               depth > 0.0f && depth <= 64.0f;
    }
    bool readPosition(void *actor, Vec3 &output) const noexcept {
        void *value = component(actor, layout_.stateVectorComponentHash,
                                layout_.stateVectorComponentHashAlternate);
        return value && world_native::read(value, layout_.statePositionOffset, output) &&
               std::isfinite(output.x) && std::isfinite(output.y) && std::isfinite(output.z);
    }
    bool readRotation(void *actor, ActorRotation &output) const noexcept {
        void *rotation = component(actor, layout_.actorRotationComponentHash,
                                    layout_.actorRotationComponentHashAlternate);
        return rotation && world_native::read(rotation, layout_.rotationPitchOffset, output.pitch) &&
               world_native::read(rotation, layout_.rotationYawOffset, output.yaw) &&
               std::isfinite(output.pitch) && std::isfinite(output.yaw);
    }

    // Resolves the local Actor's registry and reports the Actor-owner pool
    // size the caller's sweep schedule plans against.
    bool registryPopulation(void *local, ActorRegistryCaptureStats &stats) const noexcept {
        void *registry = registryOf(local);
        if (!registry) {
            stats.failure = "local_registry_unreadable";
            return false;
        }
        __try {
            using Registry = entt::basic_registry<world_native::Entity>;
            auto *typed = static_cast<Registry *>(registry);
            stats.stage = "lookup_actor_owner_storage";
            auto *owner = typed->storage(layout_.actorOwnerComponentHash);
            if (!owner) {
                stats.failure = "actor_owner_storage_missing";
                return false;
            }
            stats.stage = "owner_pool_size";
            stats.population = owner->size();
            if (stats.population > 16384) {
                stats.failure = "owner_pool_size_out_of_range";
                return false;
            }
            return true;
        } __except (captureFault(GetExceptionInformation(), stats)) {
            return false;
        }
    }

    // Incremental walk over the Actor-owner pool starting at `cursor`. The
    // caller owns sweep scheduling: it passes how many entries this tick may
    // visit and receives the visited count back. Samples are appended up to
    // sampleCapacity; the walk also stops after a cooperative 250us budget.
    bool captureBatch(void *local, const ActorRegistryCapturePolicy &policy,
                      std::size_t visitLimit, std::size_t &cursor, ActorRegistrySample *samples,
                      std::size_t sampleCapacity, std::size_t &sampleCount, std::size_t &visited,
                      ActorRegistryCaptureStats &stats) const noexcept {
        sampleCount = 0;
        visited = 0;
        void *registry = registryOf(local);
        if (!registry) {
            stats.failure = "local_registry_unreadable";
            return false;
        }
        __try {
            using Registry = entt::basic_registry<world_native::Entity>;
            auto *typed = static_cast<Registry *>(registry);
            const auto &layout = layout_;
            stats.stage = "lookup_actor_owner_storage";
            auto *owner = typed->storage(layout.actorOwnerComponentHash);
            if (!owner) {
                stats.failure = "actor_owner_storage_missing";
                return false;
            }
            stats.stage = "lookup_runtime_id_storage";
            auto *runtime = typed->storage(layout.runtimeIdComponentHash);
            auto *unique = typed->storage(layout.actorUniqueIdComponentHash);
            if (!runtime) {
                stats.failure = "runtime_id_storage_missing";
                return false;
            }
            stats.stage = "lookup_aabb_storage";
            auto *shape = typed->storage(layout.aabbShapeComponentHash);
            stats.stage = "lookup_actor_type_storage";
            auto *actorType = typed->storage(layout.actorTypeComponentHash);
            auto *candidates = owner;
            // A fixed source keeps the incremental cursor stable when pool sizes change.
            stats.stage = "owner_pool_size";
            stats.population = candidates->size();
            if (stats.population > 16384) {
                stats.failure = "owner_pool_size_out_of_range";
                return false;
            }
            if (!stats.population) {
                cursor = 0;
                return true;
            }
            const auto begin = std::chrono::steady_clock::now();
            for (std::size_t n = 0; n < visitLimit; ++n) {
                // Cooperative 250us budget; a single native read cannot be preempted.
                if (n && std::chrono::steady_clock::now() - begin >= std::chrono::microseconds(250))
                    break;
                if (cursor >= stats.population)
                    cursor = 0;
                stats.stage = "owner_pool_entry";
                const auto entity = candidates->data()[cursor++];
                ++visited;
                stats.stage = "validate_registry_entity";
                if (!typed->valid(entity) || !owner->contains(entity))
                    continue;
                stats.stage = "read_runtime_id_component";
                std::uint64_t runtimeId = 0;
                if (!runtime->contains(entity) ||
                    !world_native::read(runtime->value(entity), layout.runtimeIdValueOffset,
                                        runtimeId) ||
                    !runtimeId) {
                    ++stats.missingRuntime;
                    continue;
                }
                stats.stage = "read_actor_type_component";
                std::uint32_t type = 0;
                const bool nativePlayer =
                    actorType && actorType->contains(entity) &&
                    world_native::read(actorType->value(entity), actorTypeValueOffset_, type) &&
                    type == playerTypeId_;
                const bool isPlayer =
                    nativePlayer ||
                    (policy.knownPlayer && policy.knownPlayer(policy.context, runtimeId));
                if (policy.playersOnly && !isPlayer) {
                    ++stats.filteredActors;
                    continue;
                }
                // Validate the owner against the exact registry/entity once, rather
                // than validating the Actor vtable for every property wrapper.
                stats.stage = "read_actor_owner_component";
                void *actor = nullptr;
                if (!world_native::read(owner->value(entity), layout.actorOwnerPointerOffset,
                                        actor) ||
                    !actor) {
                    ++stats.invalidOwner;
                    continue;
                }
                // Exclude self before AABB/name reads, tracking and retention.
                if (actor == local)
                    continue;
                stats.stage = "validate_actor_context";
                void *actorRegistry = nullptr;
                std::uint32_t actorEntity = 0;
                if (!world_native::read(
                        actor, layout.actorEntityContextOffset + layout.contextRegistryOffset,
                        actorRegistry) ||
                    actorRegistry != registry ||
                    !readEntityId(actor, actorEntity) ||
                    actorEntity != static_cast<std::uint32_t>(entity)) {
                    ++stats.invalidOwner;
                    continue;
                }
                if (nativePlayer) {
                    if (policy.classifyPlayer)
                        policy.classifyPlayer(policy.context, runtimeId);
                    ++stats.nativePlayers;
                } else if (isPlayer)
                    ++stats.rememberedPlayers;
                stats.stage = "read_aabb_component";
                if (!shape || !shape->contains(entity)) {
                    ++stats.missingShape;
                    continue;
                }
                if (sampleCount >= sampleCapacity)
                    break;
                auto &sample = samples[sampleCount];
                sample.isPlayer = isPlayer;
                sample.runtimeId = runtimeId;
                sample.actorKey = reinterpret_cast<std::uintptr_t>(actor);
                sample.entityId = static_cast<std::uint32_t>(entity);
                sample.uniqueId = 0;
                if (unique && unique->contains(entity))
                    world_native::read(unique->value(entity), layout.actorUniqueIdValueOffset,
                                       sample.uniqueId);
                auto *aabb = shape->value(entity);
                if (layout.aabbMaximumOffset == layout.aabbMinimumOffset + sizeof(Vec3)) {
                    if (!world_native::read(aabb, layout.aabbMinimumOffset, sample.bounds)) {
                        ++stats.invalidBounds;
                        continue;
                    }
                } else if (!world_native::read(aabb, layout.aabbMinimumOffset,
                                               sample.bounds.minimum) ||
                           !world_native::read(aabb, layout.aabbMaximumOffset,
                                               sample.bounds.maximum)) {
                    ++stats.invalidBounds;
                    continue;
                }
                if (!sample.bounds.valid()) {
                    ++stats.invalidBounds;
                    continue;
                }
                const auto &b = sample.bounds;
                if (b.maximum.x - b.minimum.x <= 0 || b.maximum.y - b.minimum.y <= 0 ||
                    b.maximum.z - b.minimum.z <= 0) {
                    ++stats.invalidBounds;
                    continue;
                }
                sample.seenAt = std::chrono::steady_clock::now();
                ++sampleCount;
            }
            stats.stage = "complete";
            return true;
        } __except (captureFault(GetExceptionInformation(), stats)) {
            return sampleCount != 0; // Publish any rows copied before the failed read.
        }
    }

    // Refreshes bounds for already discovered handles in place: handles whose
    // runtime identity no longer matches are swap-removed from the handle
    // list, survivors are copied to `copies`. Returns the copied count.
    std::size_t copyTrackedPoses(void *local, ActorRegistrySample *handles, std::size_t &count,
                                 ActorRegistrySample *copies) const noexcept {
        void *registry = registryOf(local);
        if (!registry)
            return 0;
        std::size_t copied = 0;
        __try {
            using Registry = entt::basic_registry<world_native::Entity>;
            auto *typed = static_cast<Registry *>(registry);
            auto *runtime = typed->storage(layout_.runtimeIdComponentHash);
            auto *shape = typed->storage(layout_.aabbShapeComponentHash);
            if (!runtime || !shape)
                return 0;
            const auto now = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < count;) {
                auto sample = handles[i];
                const world_native::Entity entity{sample.entityId};
                std::uint64_t id = 0;
                if (!typed->valid(entity) || !runtime->contains(entity) ||
                    !world_native::read(runtime->value(entity), layout_.runtimeIdValueOffset,
                                        id) ||
                    id != sample.runtimeId) {
                    handles[i] = handles[--count];
                    continue;
                }
                ++i;
                if (!shape->contains(entity))
                    continue;
                auto *value = shape->value(entity);
                if (layout_.aabbMaximumOffset == layout_.aabbMinimumOffset + sizeof(Vec3)) {
                    if (!world_native::read(value, layout_.aabbMinimumOffset, sample.bounds))
                        continue;
                } else if (!world_native::read(value, layout_.aabbMinimumOffset,
                                               sample.bounds.minimum) ||
                           !world_native::read(value, layout_.aabbMaximumOffset,
                                               sample.bounds.maximum))
                    continue;
                const auto &b = sample.bounds;
                if (!b.valid() || b.maximum.x <= b.minimum.x || b.maximum.y <= b.minimum.y ||
                    b.maximum.z <= b.minimum.z)
                    continue;
                sample.seenAt = now;
                handles[i - 1] = sample;
                copies[copied++] = sample;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return copied;
    }

private:
    static int captureFault(EXCEPTION_POINTERS *exception,
                            ActorRegistryCaptureStats &stats) noexcept {
        stats.failure = "guarded_native_exception";
        if (exception && exception->ExceptionRecord) {
            stats.faultCode = exception->ExceptionRecord->ExceptionCode;
            stats.faultAddress =
                reinterpret_cast<std::uintptr_t>(exception->ExceptionRecord->ExceptionAddress);
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }

    NativeActorLayout layout_{};
    std::size_t actorTypeValueOffset_{};
    std::uint32_t playerTypeId_{};
};
} // namespace bedrock
