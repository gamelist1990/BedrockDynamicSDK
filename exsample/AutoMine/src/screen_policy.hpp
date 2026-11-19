#pragma once
#include <string_view>

namespace automine {
inline bool userScreenActive(std::string_view screen) {
    // Unknown/failed screen reads must not authorize a native UI transition.
    return screen != "hud_screen" && screen != "hud_crosshair_screen";
}
inline bool screenBlocksMovement(std::string_view screen, bool continueInChat) {
    if (continueInChat && screen.find("chat") != std::string_view::npos)
        return false;
    return userScreenActive(screen);
}
} // namespace automine
