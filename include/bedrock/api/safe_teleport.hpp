#pragma once
#include <bedrock/enums/teleport.hpp>
#include <chrono>
#include <functional>
#include <optional>
#include <stdexcept>

namespace bedrock {


struct TeleportResult {
    TeleportPhase phase;
    TeleportError error{TeleportError::None};
    std::optional<float> feetY;
};
struct SafeTeleportOptions {
    unsigned maximumRise{}; // zero: dimension height is the only distance limit
    unsigned levelsPerTick{8};
    std::chrono::microseconds budget{500};
    void validate() const {
        if (maximumRise > 65535 || levelsPerTick < 1 || levelsPerTick > 32 ||
            budget < std::chrono::microseconds{100} || budget > std::chrono::microseconds{2000})
            throw std::invalid_argument("invalid safe teleport distance or tick budget");
    }
};
} // namespace bedrock
