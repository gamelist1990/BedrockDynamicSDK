#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace bedrock::detail {
struct MaterialUvLayout {
    std::size_t sourceStride{}, targetStride{}, sourceCoordinates{}, sourceWeight{}, sourcePbr{};
    std::size_t targetCoordinates{}, targetWeight{}, targetPbr{}, writeBytes{};
};
// Plan on owned copies. Preserve all target fields except the native ctor's UV,
// weight and PBR fields; never resize a game's vector or replace its shared_ptr.
inline bool planMaterialUvs(const MaterialUvLayout &l, std::span<const std::byte> source,
                            std::span<const std::byte> target, std::vector<std::byte> &result) {
    if (!l.sourceStride || !l.targetStride || l.writeBytes > l.targetStride ||
        l.sourceCoordinates > l.sourceStride || l.sourceStride - l.sourceCoordinates < 16 ||
        l.sourceWeight > l.sourceStride || l.sourceStride - l.sourceWeight < 4 ||
        l.sourcePbr > l.sourceStride || l.sourceStride - l.sourcePbr < 2 ||
        l.targetCoordinates > l.writeBytes || l.writeBytes - l.targetCoordinates < 16 ||
        l.targetWeight > l.writeBytes || l.writeBytes - l.targetWeight < 4 ||
        l.targetPbr > l.writeBytes || l.writeBytes - l.targetPbr < 2 ||
        source.empty() || source.size() % l.sourceStride || target.size() % l.targetStride ||
        source.size() / l.sourceStride != target.size() / l.targetStride)
        return false;
    auto next = std::vector<std::byte>(target.begin(), target.end());
    for (std::size_t i = 0; i < source.size() / l.sourceStride; ++i) {
        const auto *s = source.data() + i * l.sourceStride;
        auto *d = next.data() + i * l.targetStride;
        std::array<float, 4> uv{};
        float weight{};
        std::memcpy(uv.data(), s + l.sourceCoordinates, sizeof(uv));
        std::memcpy(&weight, s + l.sourceWeight, sizeof(weight));
        if (!std::isfinite(weight) || weight < 0)
            return false;
        for (auto coordinate : uv)
            if (!std::isfinite(coordinate) || coordinate < -0.001f || coordinate > 1.001f)
                return false;
        if (uv[2] < uv[0] || uv[3] < uv[1])
            return false;
        std::memcpy(d + l.targetCoordinates, uv.data(), sizeof(uv));
        std::memcpy(d + l.targetWeight, &weight, sizeof(weight));
        std::memcpy(d + l.targetPbr, s + l.sourcePbr, sizeof(std::uint16_t));
    }
    result.swap(next);
    return true;
}
} // namespace bedrock::detail
