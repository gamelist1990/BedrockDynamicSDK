#pragma once

namespace bedrock::detail::oreui_settings::impl {
// This is the set shared by the keyboard capture UI and the input adapter.
// Escape restores the default during capture, so it cannot be assigned.
constexpr bool validModKey(unsigned code) noexcept {
    return code == 9 || code == 32 || (code >= '0' && code <= '9') ||
           (code >= 'A' && code <= 'Z') || (code >= 112 && code <= 123);
}
}
