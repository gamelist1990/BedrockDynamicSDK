#pragma once
#include <cmath>
#include <stdexcept>

namespace bedrock {
class Gamma {
public:
    explicit Gamma(float value) : value_(value) {
        if (!std::isfinite(value) || value < 0.f || value > 25.f)
            throw std::invalid_argument("gamma must be finite and in 0..25");
    }
    float value() const noexcept { return value_; }
    static Gamma fullBright() { return Gamma{25.f}; }
private:
    float value_;
};
} // namespace bedrock
