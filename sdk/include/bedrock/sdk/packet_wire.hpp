#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "profiles.hpp"

namespace bedrock::sdk {

// One intercepted outgoing packet, copied out of the native send call.
// `bytes` is the complete wire frame (header + payload) and may be rewritten
// by the handler; only the payload is spliced back, the original header is
// preserved.
struct PacketWireEvent {
    void* sender{};
    int packetId{};
    std::string bytes;
    // False when the native frame could not be copied safely. `bytes` is then
    // empty and the handler can only decide whether to keep the send.
    bool readable{};
    // Set by the handler to drop the outgoing packet.
    bool cancelled{};
};

// --------------------------------------------------------------------------
// PacketWire — public facade over the SDK's pre-batch packet send bridge.
// It owns the shared hook subscription (both the send-to-server attribution
// point and the wire bridge), so Mods never see hook tokens, native argument
// structs or the hook continuation protocol.
// --------------------------------------------------------------------------
class PacketWire final {
public:
    // Invoked once per intercepted outgoing packet on the game's network
    // thread. Must not block; `cancelled` decides whether the packet is sent.
    using Handler = void (*)(PacketWireEvent& event, void* user);

    PacketWire();
    ~PacketWire();
    PacketWire(const PacketWire&) = delete;
    PacketWire& operator=(const PacketWire&) = delete;
    PacketWire(PacketWire&&) noexcept;
    PacketWire& operator=(PacketWire&&) noexcept;

    // Resolve and validate the exact-build wire bridge from the selected
    // profile. Must be called before install().
    bool load(const Profile& profile, std::string& error);

    // Wire bridge entry resolved from the profile, or null before load().
    void* wireTarget() const noexcept;

    // True when `target` can be hooked as the send-to-server attribution
    // point: either the shared runtime already owns it, or it is a genuine
    // function entry. Used to confirm a live sender's vtable slot before
    // subscribing.
    bool accepts(void* sendTarget) const noexcept;

    // Subscribe the wire bridge plus the send-to-server attribution point at
    // `sendTarget`. Replaces any previously installed handler.
    bool install(void* sendTarget, Handler handler, void* user, std::string& error);

    // Unsubscribe. Safe to call more than once.
    void stop() noexcept;

    bool installed() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bedrock::sdk
