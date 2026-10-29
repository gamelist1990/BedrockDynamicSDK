#pragma once
#include <cstdint>
#include <cstddef>

namespace bedrock {
enum class TeleportError {
    None, InvalidDistance, DistanceLimit, UnavailableContext, HeightLimit,
    NoSearchSpace, WorldChanged, PlayerMoved, ChunkUnavailable, NoSafeDestination,
    BodyObstructed, UnsupportedHeight, MissingFloor, UnsafeFloor, UnsupportedFloor,
    NativeWriteFailed, InternalError
};

enum class TeleportPhase { Searching, Moved, Failed };
} // namespace bedrock
