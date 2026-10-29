#pragma once
#include <cstdint>
#include <cstddef>

namespace bedrock {
enum class GameType : std::int32_t { Survival = 0, Creative = 1, Adventure = 2, Default = 5, Spectator = 6 };

enum class GameTypeChange { Applied, Unsupported, InvalidType, NoPlayer, WrongThread, WorldChanged, NativeFailure };
} // namespace bedrock
