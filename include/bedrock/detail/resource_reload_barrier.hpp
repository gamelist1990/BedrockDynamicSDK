#pragma once
#include <cstdint>
#include <string_view>

namespace bedrock::detail {
inline bool isWorldRenderScreen(std::string_view screen) noexcept {
    return screen == "hud_screen" || screen == "pause_screen";
}
// Record a completed original frame, not elapsed time. A request, loader, atlas,
// menu transition or renewed work invalidates the observation.
class ResourceReloadBarrier {
    std::uint64_t generation_{};
    const void *loader_{}, *atlas_{};
    bool idle_{};
  public:
    void clear() noexcept { idle_ = false; loader_ = atlas_ = nullptr; }
    bool allows(std::uint64_t generation, const void *loader, const void *atlas,
                bool idle) const noexcept {
        return idle && idle_ && loader && atlas && generation_ == generation &&
            loader_ == loader && atlas_ == atlas;
    }
    void observe(std::uint64_t generation, const void *loader, const void *atlas,
                 bool idle) noexcept {
        clear();
        if (idle && loader && atlas) {
            generation_ = generation; loader_ = loader; atlas_ = atlas; idle_ = true;
        }
    }
};
} // namespace bedrock::detail
