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
} // namespace bedrock
