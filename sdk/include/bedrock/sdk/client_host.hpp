#pragma once

#include "mod.hpp"
#include <functional>
#include <memory>

namespace bedrock { class RuntimeProfile; }

namespace bedrock::sdk {

class BrightnessOverride;

// Own alongside the Mod (the Mod must outlive its host). Start/stop must run
// from the loader thread, outside callbacks and outside the DLL loader lock.
class ClientHost {
public:
    using ErrorHandler = std::function<void(const Error&)>;
    ClientHost();
    ~ClientHost();
    ClientHost(const ClientHost&) = delete;
    ClientHost& operator=(const ClientHost&) = delete;

    [[nodiscard]] Result<void> start(Mod& mod, ErrorHandler onError = {});
    [[nodiscard]] Result<void> stop();

    // Stage B (D-2): additional Mods share the already-started host's tick,
    // WorldSession and GameFrameScope. attach() enables the Mod before it
    // starts receiving callbacks; detach() stops delivery and disables it.
    // Both must run from the loader thread, outside callbacks (like start).
    [[nodiscard]] Result<void> attach(Mod& mod);
    [[nodiscard]] Result<void> detach(Mod& mod);
private:
    friend class BrightnessOverride;
    std::shared_ptr<const RuntimeProfile> profileSnapshot() const;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bedrock::sdk
