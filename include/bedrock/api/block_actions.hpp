#pragma once
#include <bedrock/enums/block_actions.hpp>
#include <cstdint>

namespace bedrock {
struct BlockActionReport {
    BlockActionFailure failure{BlockActionFailure::None};
    unsigned virtualSlot{};
    std::uint32_t observedFunctionRva{};
    bool called{}, accepted{};
    float progressBefore{};
    std::uint64_t idleStopsSuppressed{};
};
inline const char* blockActionFailureText(BlockActionFailure failure) noexcept {
    switch (failure) {
    case BlockActionFailure::None: return "accepted";
    case BlockActionFailure::NotLoaded: return "adapter not loaded";
    case BlockActionFailure::InvalidFace: return "invalid target face";
    case BlockActionFailure::GameModeUnavailable: return "player GameMode unavailable";
    case BlockActionFailure::VirtualFunctionMismatch: return "GameMode virtual function not in checked bindings";
    case BlockActionFailure::HitContextUnavailable: return "block hit context unavailable";
    case BlockActionFailure::NativeFault: return "native call/hit context fault";
    case BlockActionFailure::GameRejected: return "game returned false";
    }
    return "unknown";
}
}
