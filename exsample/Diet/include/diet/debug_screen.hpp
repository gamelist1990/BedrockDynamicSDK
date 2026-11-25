#pragma once
// Diet — F3 debug screen (Java-Edition-style overlay).
//
// The screen is drawn through sdk::Overlay, the same Present-thread drawing
// surface the other Mods' overlays use. Layout and formatting live in
// debug_layout.hpp (host-testable); this class owns the native overlay and the
// last published snapshot.
#include "diet/debug_layout.hpp"

#include <memory>
#include <string>

namespace diet {

class DebugScreen {
public:
    DebugScreen();
    ~DebugScreen();
    DebugScreen(const DebugScreen&) = delete;
    DebugScreen& operator=(const DebugScreen&) = delete;

    // Installs the present-thread overlay. Independent of every Diet feature:
    // failure only disables the screen.
    bool install(std::string& error);
    void uninstall() noexcept;

    void setVisible(bool visible) noexcept;
    [[nodiscard]] bool visible() const noexcept;
    void toggle() noexcept;
    // Java only shows the debug screen inside a world. The Mod mirrors that by
    // gating the paint on the gameplay scope.
    void setWorldActive(bool active) noexcept;

    // Diagnostics: how many times the present-thread draw callback ran. Used
    // by the 5 s log line to distinguish "screen off" from "overlay not
    // drawing" without logging on the render thread.
    [[nodiscard]] unsigned paintCount() const noexcept;

    // Swaps the displayed snapshot. Called from the housekeeping thread.
    void publish(std::shared_ptr<const F3Snapshot> snapshot) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Static system description plus current memory figures for the right column.
// Safe on a worker thread; no game state is read.
[[nodiscard]] F3SystemInfo collectSystemInfo();

}  // namespace diet
