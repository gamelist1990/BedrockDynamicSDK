#pragma once
#include <algorithm>
#include <cmath>

namespace bedrock {
// Automation owns an intent, never the user's keyboard or camera state.
// MoveInput's X axis is left; Y is forward in the current camera basis.
struct AutomationIntent {
    float forward{}, left{}, yaw{}, pitch{};
    bool jump{}, sneak{}, sprint{}, active{}, look{};
    // Sprint-jump locomotion needs engine-forward input and a boost along yaw,
    // not camera-relative strafing. Native adapters borrow actor yaw for simulation only.
    bool sprintJump{};
};
struct AutomationInputObservation {
    unsigned long long sequence{}, sprintCalls{};
    bool applied{}, sprintJump{};
    float forward{}, left{}, yaw{};
    unsigned input{}, held{}, sprintMask{};
};
struct AutomationMove { float left{}, forward{}; };
inline float automationSimulationYaw(const AutomationIntent& intent, float cameraYaw) noexcept {
    return intent.sprintJump ? intent.yaw : cameraYaw;
}
inline AutomationMove automationMove(const AutomationIntent& intent, float cameraYaw) noexcept {
    if (!std::isfinite(intent.yaw) || !std::isfinite(cameraYaw) ||
        !std::isfinite(intent.forward) || !std::isfinite(intent.left)) return {};
    const double angle = (intent.yaw - cameraYaw) * 3.141592653589793 / 180.0;
    const float f = std::clamp(intent.forward, -1.f, 1.f);
    const float l = std::clamp(intent.left, -1.f, 1.f);
    AutomationMove result{static_cast<float>(l * std::cos(angle) - f * std::sin(angle)),
                          static_cast<float>(f * std::cos(angle) + l * std::sin(angle))};
    const float length = std::hypot(result.left, result.forward);
    if (length > 1.f) { result.left /= length; result.forward /= length; }
    return result;
}
} // namespace bedrock
