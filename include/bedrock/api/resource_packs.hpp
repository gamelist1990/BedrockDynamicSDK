#pragma once

#include <string>
#include <string_view>

namespace bedrock {

// Native UI observations only; no native pointers or version-specific layout.
// Keep one policy per ClientInstance and serialize access on the adapter side.
class ResourcePackMenuPolicy {
  public:
    bool preGame(bool original, std::string_view screen, bool inWorld) {
        if (!inWorld) {
            reset();
            return original;
        }
        const bool settings =
            screen.find("screen_world_controls_and_settings") != std::string_view::npos;
        if (settings)
            globalTab_ = screen.find("global_texture_pack_tab") != std::string_view::npos;
        const bool transient = screen == "screen" || screen == "progress_popup";
        const bool global =
            settings && screen.find("global_texture_pack_tab") != std::string_view::npos;
        const bool result = original || global || (globalTab_ && transient);
        if (!transient && !settings)
            globalTab_ = false;
        return result;
    }
    void reset() noexcept {
        globalTab_ = false;
    }

  private:
    bool globalTab_{};
};
} // namespace bedrock
