#pragma once
#include <memory>
#include <string>
#include <string_view>

namespace bedrock::sdk {

// ------------------------------------------------------------------------
// OverlayOptions — options for sdk::Overlay::install().
// Mirrors the subset of bedrock::gui::OverlayOptions used by TabList.
// ------------------------------------------------------------------------
struct OverlayOptions {
    unsigned toggleVirtualKey = 0x79;  // F10 default; 0 disables toggle key
    bool initiallyVisible = true;
    bool captureInput = true;
    bool nativeCursor = true;
};

// ------------------------------------------------------------------------
// Overlay — opaque facade wrapping bedrock::gui::Overlay.
//
//   sdk::Overlay overlay;
//   sdk::OverlayOptions options;
//   options.toggleVirtualKey = 0;
//   options.captureInput = false;
//   std::string error;
//   if (!overlay.install(&drawCallback, nullptr, options, error)) { ... }
//   overlay.uninstall();
//
// The draw callback receives an opaque render context (forward-declared
// ImGuiContext* is acceptable — overlay.cpp draws with ImGui directly).
// No internal gui types leak into this header.
// ------------------------------------------------------------------------
class Overlay final {
public:
    Overlay();
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;
    Overlay(Overlay&&) noexcept;
    Overlay& operator=(Overlay&&) noexcept;

    // Install a draw callback. The callback is invoked ON THE GAME PRESENT
    // THREAD, once per presented frame.  The `userdata` pointer is passed
    // to the callback as its first argument.
    bool install(void (*drawCallback)(void* userdata),
                 void* userdata,
                 const OverlayOptions& options,
                 std::string& error);

    // Uninstall the overlay (mirrors bedrock::gui::Overlay::shutdown).
    void uninstall() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bedrock::sdk