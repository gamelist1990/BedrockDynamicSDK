#pragma once
// OPTIONAL Windows native GUI host. Not a Minecraft swap-chain hook.
// It creates its own Win32/D3D11 window and pumps its own messages on the
// thread that calls run(), which should NOT be DllMain.
#include <functional>
#include <string>

namespace bedrock::gui {
struct HostOptions {
    std::wstring title = L"Bedrock GUI";
    int width = 1080;
    int height = 720;
    bool vsync = true;
    bool alwaysOnTop = false;
};
class GuiHost {
public:
    using FrameHandler = std::function<void()>;
    // Blocks until the tool window closes; invoke on a client-owned UI thread.
    // If the render backend fails, returns false with error.
    static bool run(const HostOptions& options, FrameHandler render,
                    std::string& error);
};
} // namespace bedrock::gui
