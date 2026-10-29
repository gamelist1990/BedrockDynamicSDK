#pragma once
#include <bedrock/enums/game_type.hpp>
#include <cstdint>
#include <optional>

namespace bedrock {
// Native Bedrock values. Spectator is 6, distinct from the Java enum value.

inline std::optional<GameType> gameTypeFromNative(std::int32_t value) noexcept {
    switch (value) {
    case 0: return GameType::Survival;
    case 1: return GameType::Creative;
    case 2: return GameType::Adventure;
    case 5: return GameType::Default;
    case 6: return GameType::Spectator;
    default: return std::nullopt;
    }
}


// Stable engine boundary. Native adapters must invoke the engine's setter;
// updating an ECS integer alone does not update abilities or GameMode state.
class GameTypeAccess {
public:
    virtual ~GameTypeAccess() = default;
    virtual std::optional<GameType> gameType() const = 0;
    virtual GameTypeChange setGameType(GameType value) = 0;
};
} // namespace bedrock
