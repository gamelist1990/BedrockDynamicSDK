#pragma once
// Optional Minecraft in-game DirectX GUI overlay.
// This is NOT the separate-window GuiHost.  It hooks the existing DXGI
// swap-chain and renders to Minecraft's own backbuffer at Present().
// Call install from an initialized mod worker, NEVER from DllMain.
#include <string>

namespace bedrock::gui {
struct OverlayOptions {
    // F10 by default; 0 disables the built-in toggle key.
    unsigned toggleVirtualKey = 0x79;
    bool initiallyVisible = true;
    bool captureInput = true;
    // Fail closed if loaded outside a Minecraft.Windows.exe process.
    bool requireMinecraftProcess = true;
};
class Overlay {
public:
    // callback is invoked ON THE GAME PRESENT THREAD, once per presented frame.
    // No custom game tick timing, polling loop, or separate render window.
    using RenderCallback = void (*)(void* userdata);
    static bool install(RenderCallback callback, void* userdata,
                        std::string& error, OverlayOptions options = {});
    static void show(bool visible) noexcept;
    static bool visible() noexcept;
    static bool running() noexcept;
    // Unhook before unloading a DLL that contains the callback.
    static void shutdown() noexcept;
};
} // namespace bedrock::gui
