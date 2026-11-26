#pragma once

#include "profiles.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bedrock::sdk {

struct PacketDeferralStatistics {
    bool ready{};
    unsigned handlers{};
    unsigned calls{};        // handler entries observed this window
    unsigned deferred{};     // packets held instead of handled
    unsigned replayed{};     // held packets handed back to the handler
    unsigned overflow{};     // held-packet queue was full; handled immediately
    unsigned otherObject{};  // entry called by an object with another vtable
    unsigned worldDrops{};   // held packets dropped on a world change
    unsigned agedOut{};      // held packets drained because they were too old
    unsigned queue{};        // packets still held
};

// Delay-tolerant packet deferral at the client-network handler entry.
//
// Only the handler overloads the profile lists are detoured (the
// UpdateBlock-family shared_ptr overloads). Every other packet class keeps the
// untouched path, so an unknown or critical id can never be intercepted. The
// adapter verifies the authenticated entry bytes, the vtable slot that names
// that entry and the callable table of the overload's own queued lambda before
// it installs anything; on any mismatch nothing is hooked and `configure`
// returns the reason.
//
// Held packets are replayed by the Mod from the game thread (`pump`), which
// keeps the level-side work under the Mod's own per-frame budget. `clear` drops
// what is held (used on a world change) so stale block updates are never
// applied to a different level.
class PacketDeferral {
public:
    PacketDeferral();
    ~PacketDeferral();
    PacketDeferral(const PacketDeferral&) = delete;
    PacketDeferral& operator=(const PacketDeferral&) = delete;
    PacketDeferral(PacketDeferral&&) noexcept;
    PacketDeferral& operator=(PacketDeferral&&) noexcept;

    bool configure(const Profile& profile, std::string& error,
                   std::vector<std::string>& warnings);
    [[nodiscard]] bool ready() const noexcept;
    void setEnabled(bool enabled) noexcept;
    [[nodiscard]] bool enabled() const noexcept;
    // Replay at most `budget` held packets (game thread). Returns the count.
    unsigned pump(unsigned budget) noexcept;
    // Drop every held packet (game thread, world change). Returns the count.
    unsigned clear() noexcept;
    [[nodiscard]] unsigned queueDepth() const noexcept;
    PacketDeferralStatistics takeStatistics() noexcept;
    // "UpdateBlock=0x6BD55A0 UpdateBlockSynced=0x6BD5420 ..." for the log.
    [[nodiscard]] std::string handlerSummary() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bedrock::sdk
