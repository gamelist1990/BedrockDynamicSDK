#pragma once

#include "freecam/config.hpp"
#include "tablist/config.hpp"
#include "zoom/config.hpp"
#include <bedrock/sdk/logging.hpp>
#include <bedrock/sdk/profiles.hpp>
#include <bedrock/sdk.hpp>
#include <filesystem>

namespace dynamic_client {
void registerSettings(const bedrock::sdk::Mod& mod, bedrock::sdk::Logger& logger);
void registerKeybinds(bedrock::sdk::Mod& mod);
unsigned boundKey(std::string_view action, bedrock::sdk::Key defaultKey);
void startZoom(const bedrock::sdk::Profile& profile, const bedrock::sdk::SymbolResolver& resolver, bedrock::sdk::Logger& logger);
void startSprint(const bedrock::sdk::Profile& profile, const bedrock::sdk::SymbolResolver& resolver, bedrock::sdk::Logger& logger);
void startBrightness(const bedrock::sdk::ClientHost& host, bedrock::sdk::Logger& logger);
void setZoomConfig(const zoom::Config& config);
void setSprintEnabled(bool enabled) noexcept;
void setBrightness(float gamma, bool enabled);
void startTabList(const bedrock::RuntimeProfile& profile, const std::filesystem::path& directory);
void setTabListConfig(const tablist::Config& config);
void onTabListKeybind(bool pressed);
void runFreecam(const bedrock::RuntimeProfile& selected, const std::filesystem::path& directory);
void setFreecamConfig(const freecam::Config& config);
void onFreecamKeybind(bool pressed);
void onZoomKeybind(bool pressed);
} // namespace dynamic_client