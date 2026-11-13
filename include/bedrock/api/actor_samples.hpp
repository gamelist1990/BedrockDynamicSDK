#pragma once
#include <bedrock/api/geometry.hpp>
#include <array>
#include <chrono>
#include <cstdint>
namespace bedrock {
struct TrackedActorBounds {
    std::uint64_t id{};
    bedrock::ActorBounds bounds{}, blendFrom{};
};
struct ActorBoundsFrame {
    std::uint64_t epoch{};
    std::chrono::steady_clock::time_point made{};
    std::array<TrackedActorBounds, 128> boxes{};
    unsigned count{};
    Vec3 origin{};
};
// One Actor copied out of the native ECS registry by NativeActorRegistry.
// Value-only: no registry, storage or component pointer survives in it, so
// workers may retain and process samples freely.
struct ActorRegistrySample {
    std::uint64_t runtimeId{};
    ActorBounds bounds{};
    std::chrono::steady_clock::time_point seenAt{};
    bool isPlayer{};
    std::uintptr_t actorKey{};
    std::uint32_t entityId{};
    std::uint64_t uniqueId{};
};
} // namespace bedrock
