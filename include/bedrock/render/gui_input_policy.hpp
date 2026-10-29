#pragma once

// Render/input-independent policy for deciding when Minecraft mouse events
// belong exclusively to an open overlay. Does not access game memory.
namespace bedrock::gui {
[[nodiscard]] constexpr bool shouldBlockGameMouse(bool overlayRunning,
                                                    bool overlayVisible,
                                                    bool captureEnabled,
                                                    bool gameFocused,
                                                    bool guiReady) noexcept {
    return overlayRunning && overlayVisible && captureEnabled &&
           gameFocused && guiReady;
}
}
