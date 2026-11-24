#pragma once

#include <filesystem>
#include <vector>

namespace bedrock::sdk {

// Process-lifetime Win32 file-drop receiver for the game's main window.
// A single consumer can poll and drain it from a worker thread. The window
// procedure only copies paths; it does not read files or invoke user code.
std::vector<std::filesystem::path> pollWindowFileDrops();

} // namespace bedrock::sdk
