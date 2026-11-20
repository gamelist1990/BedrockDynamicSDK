#pragma once
#include <cmath>
namespace bedrock {
struct Vec3 {
    float x{}, y{}, z{};
};
struct ActorBounds {
    Vec3 minimum;
    Vec3 maximum;
    [[nodiscard]] bool valid() const noexcept {
        const auto finite = [](float value) {
            return std::isfinite(value) && std::abs(value) < 1.0e7f;
        };
        return finite(minimum.x) && finite(minimum.y) && finite(minimum.z) && finite(maximum.x) &&
               finite(maximum.y) && finite(maximum.z) && minimum.x <= maximum.x &&
               minimum.y <= maximum.y && minimum.z <= maximum.z;
    }
};
struct ActorRotation {
    float pitch{}, yaw{};
};

} // namespace bedrock
