#pragma once
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "resolver.hpp"

namespace bedrock::sdk {

// ------------------------------------------------------------------------
// RosterPlayerEntry — value-type snapshot of a single player from the
// PlayerRosterService worker lane. Mirrors the subset of PacketPlayer
// needed by TabList (name, UUID, face).
// ------------------------------------------------------------------------
struct RosterPlayerEntry {
    std::string name;
    std::array<std::byte, 16> uuid{};
    std::int64_t uniqueId{};
    std::array<std::uint32_t, 64> face{};
    bool hasFace{};
};

// ------------------------------------------------------------------------
// PacketRoster — TitleObserver-style packet subscriber for PlayerRoster.
//
//   sdk::PacketRoster roster;
//   std::string error;
//   if (!roster.install(resolver, profile.json(), error)) { ... }
//   auto snapshot = roster.snapshotShared();
//   roster.stop();
//
// Wraps bedrock::PacketAPI + bedrock::PlayerRosterService behind an opaque
// Impl, exposing only value-type entries.  No internal packet/wire/native
// types leak into this header.
// ------------------------------------------------------------------------
class PacketRoster final {
public:
    PacketRoster();
    ~PacketRoster();
    PacketRoster(const PacketRoster&) = delete;
    PacketRoster& operator=(const PacketRoster&) = delete;
    PacketRoster(PacketRoster&&) noexcept;
    PacketRoster& operator=(PacketRoster&&) noexcept;

    // Install from a fully-populated selected profile JSON (e.g. from
    // sdk::Profile::json()).  The profile must already be validated.
    bool install(std::string_view selectedProfileJson, std::string_view embeddedProfileJson, std::string& error);

    // Install from a resolver + profile (mirrors the pattern used by
    // sdk::Hooks::installNativeKeyboard — convenient when the caller
    // already holds a SymbolResolver).  The profile JSON is passed
    // separately so this works with the (resolver, profileJson) pair.
    bool install(const bedrock::sdk::SymbolResolver& resolver, std::string_view profileJson, std::string& error);

    // Shared snapshot of value-type roster players.  Returns a shared_ptr
    // to a vector of RosterPlayerEntry, so the caller can cheaply hold a
    // reference without copying.
    [[nodiscard]] std::shared_ptr<const std::vector<RosterPlayerEntry>> snapshotShared() const noexcept;

    // Explicit stop — mirrors PacketAPI::stop + PlayerRosterService::stop.
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bedrock::sdk