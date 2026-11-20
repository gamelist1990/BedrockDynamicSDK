#pragma once
#include <bedrock/enums/world.hpp>
#include <cstdint>

namespace bedrock {



// Handles are borrowed, and may be null (especially during exit). Do not keep
// them in worker jobs: copy scalars and use epoch to discard old-world work.
struct WorldChangeEvent {
    WorldChangeReason reason{};
    void* clientInstance{};
    void* level{};
    void* player{};
    std::uint64_t epoch{};
};
struct ClientJoinLevelEvent : WorldChangeEvent {};
struct ClientExitLevelEvent : WorldChangeEvent {};
struct ClientCancelJoinLevelEvent : WorldChangeEvent {};
// A native notification, not a guessed change in the Level pointer. Minecraft
// can change dimension while keeping that pointer. This callback has no IDs.
struct DimensionChangeEvent : WorldChangeEvent {};

} // namespace bedrock
