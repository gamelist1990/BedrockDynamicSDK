#pragma once
#include <bedrock/detail/automation_motion.hpp>
#include <array>
#include <cstdint>

namespace bedrock::detail {
// A simulation-only axis (borrowed yaw) must never escape the simulation,
// even when the engine writes it. Ordinary input axes retain engine changes.
inline float automationRestoredAxis(float live, float before, float injected,
                                    bool simulationOnly) {
    return simulationOnly || live == injected ? before : live;
}
struct AutomationFlags {
    std::uint32_t input{}, held{}, ownedMask{}, heldMask{};
};
inline AutomationFlags automationFlags(const AutomationIntent &intent, AutomationMove move,
                                       std::uint32_t previous, std::uint32_t heldPrevious,
                                       bool lastJump, bool lastSneak,
                                       const std::array<std::uint32_t, 15> &masks) {
    AutomationFlags result;
    for (auto mask : masks) result.ownedMask |= mask;
    const bool wanted[]{move.forward > .01f, move.forward < -.01f,
        move.left > .01f, move.left < -.01f, intent.jump, intent.sneak,
        intent.sprint, move.forward > .01f && move.left > .01f,
        move.forward > .01f && move.left < -.01f,
        move.forward < -.01f && move.left > .01f,
        move.forward < -.01f && move.left < -.01f,
        intent.jump && !lastJump, !intent.jump && lastJump,
        intent.sneak && !lastSneak, !intent.sneak && lastSneak};
    result.input = previous & ~result.ownedMask;
    for (std::size_t i = 0; i < masks.size(); ++i)
        if (wanted[i]) result.input |= masks[i];
    const auto edges = masks[11] | masks[12] | masks[13] | masks[14];
    result.heldMask = result.ownedMask & ~edges;
    result.held = (heldPrevious & ~result.heldMask) | (result.input & result.heldMask);
    return result;
}
} // namespace bedrock::detail
