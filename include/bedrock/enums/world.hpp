#pragma once
#include <cstdint>
#include <cstddef>

namespace bedrock {
enum class WorldChangeReason : std::uint8_t {
    Joined, Left, JoinCancelled, DimensionChanged
};

enum class CellKind { unknown, air, block };

enum class Face { down, up, north, south, west, east };

enum class Hand { main };

enum class BlockError { none, unsupported, wrongThread, staleWorld, budgetExceeded,
    unavailable, invalidRequest, missingItem, stateMismatch, nativeRejected };
} // namespace bedrock
