#pragma once

#include "client_metrics.hpp"

#include <string>
#include <string_view>

namespace bedrock::sdk {

// ------------------------------------------------------------------------
// ClientInfo — client diagnostics sampler for FPS, ping, CPS and the
// device/session description.
//
//   sdk::ClientInfo::install(selectedProfileJson, embeddedProfileJson, error);
//   const auto metrics = sdk::ClientInfo::snapshot();
//
// ClientHost::start() installs the sampler best-effort for every host, so a
// Mod normally only reads Player::fps() / ping() / cps() / device(). Install
// is reference counted: one sampler per process is shared by all callers and
// the hooks are released when the last owner calls stop().
//
// Sources (all existing 1.26.65.1 profile entries, no Mod-side RVA):
//   fps    LevelRenderer::render_level, via the shared render-level hook
//          (the same per-frame path the world renderer uses).
//   ping   NetworkStackLatencyPacket (protocol id 115) observed on the
//          packet wire send and packet read hooks; the client's own request
//          timestamp is matched to the reply. Observation only.
//   cps    MouseDevice::feed through the shared input hook; non-synthetic
//          left/right button presses.
//   device selected profile target version, platform, and the game window's
//          client area.
// ------------------------------------------------------------------------
class ClientInfo final {
public:
    ClientInfo() = delete;

    // Resolve the samplers from an already validated profile. `selected` is
    // the profile in use; `embedded` is the bundled definition used when the
    // selected profile lacks an adapter layout. Never throws; on failure
    // `error` explains which parts are unavailable.
    static bool install(std::string_view selectedProfileJson,
                        std::string_view embeddedProfileJson,
                        std::string& error);

    // Release this caller's reference. Hooks are removed with the last one.
    static void stop() noexcept;

    [[nodiscard]] static bool installed() noexcept;

    // Full snapshot. Never blocks on the render/network/input threads.
    [[nodiscard]] static ClientMetricsSnapshot snapshot() noexcept;

    // Cheap scalar reads for the per-frame path (no allocation).
    [[nodiscard]] static double fps() noexcept;
    [[nodiscard]] static PingSnapshot ping() noexcept;
    [[nodiscard]] static CpsSnapshot cps() noexcept;

    // Static-ish description; the window size is refreshed at most every 5 s.
    [[nodiscard]] static DeviceInfo device() noexcept;
};

} // namespace bedrock::sdk
