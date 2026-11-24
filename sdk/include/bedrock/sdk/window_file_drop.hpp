#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

namespace bedrock::sdk {

// Process-lifetime Win32 file-drop receiver for the game's main window.
// A single consumer can poll and drain it from a worker thread. The window
// procedure only copies paths; it does not read files or invoke user code.
std::vector<std::filesystem::path> pollWindowFileDrops();
// Single-consumer alternative: ignore releases outside the registered page's
// current drop zone, including when its geometry is unknown or stale.
std::vector<std::filesystem::path> pollWindowFileDropsForZone(std::string_view pageId);

} // namespace bedrock::sdk
