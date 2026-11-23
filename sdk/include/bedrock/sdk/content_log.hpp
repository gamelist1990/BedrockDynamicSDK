#pragma once

#include "logging.hpp"

namespace bedrock::sdk {

// Direct, best-effort output to the game's Creator Content Log.
class ContentLog final {
public:
    ContentLog() = delete;

    // True when this SDK instance has an installed writer. ClientHost attempts
    // to install it before Mod::onEnable() on supported game builds. This is
    // not a check of game settings, host readiness, or message delivery.
    [[nodiscard]] static bool available() noexcept;

    // Sends only to Content Log, without Logger filtering or SDK file/debug
    // sinks. No-op without a writer or at LogLevel::Off. The game may still
    // drop the line; this call does not acknowledge delivery.
    static void write(LogLevel level, std::string_view tag,
                      std::string_view message) noexcept;
};

} // namespace bedrock::sdk
