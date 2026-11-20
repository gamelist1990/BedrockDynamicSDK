#pragma once

#include "result.hpp"

#include <memory>
#include <optional>

namespace bedrock::sdk {

class ClientHost;

// Owns one client-side display override. It never writes the player's options.
class BrightnessOverride {
public:
    BrightnessOverride();
    ~BrightnessOverride();
    BrightnessOverride(const BrightnessOverride&) = delete;
    BrightnessOverride& operator=(const BrightnessOverride&) = delete;

    // Initialize against a started host on the loader thread, outside the DLL
    // loader lock. The host's exact profile was checked before installing hooks.
    [[nodiscard]] Result<void> connect(const ClientHost& host);
    // Use nullopt to restore the client's normal gamma for this owner.
    [[nodiscard]] Result<void> setGamma(std::optional<float> gamma);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bedrock::sdk
