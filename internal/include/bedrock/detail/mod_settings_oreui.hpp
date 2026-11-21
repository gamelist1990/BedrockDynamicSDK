#pragma once

#include "bedrock/detail/mod_settings_store.hpp"

namespace bedrock::detail::oreui_settings::impl {
inline constexpr std::string_view menu_route_prefix = "bedrockdynamic-mod-";

// The caller isolates requests to the local settings bridge.
std::optional<std::string> settingsResource(Store& store, std::string_view endpoint);
} // namespace bedrock::detail::oreui_settings::impl
