#include "bedrock/sdk/packets.hpp"
#include "resolver_access.hpp"
#ifdef _WIN32
#include <bedrock/packets/packet_api.hpp>
#include <bedrock/packets/player_roster_service.hpp>
#endif
#include <memory>
#include <string>
#include <utility>

namespace bedrock::sdk {

struct PacketRoster::Impl {
#ifdef _WIN32
    bedrock::PlayerRosterService rosterService{};
    bedrock::PacketAPI packetAPI{};
    bool installed{};
#endif
};

PacketRoster::PacketRoster() : impl_(std::make_unique<Impl>()) {}
PacketRoster::~PacketRoster() = default;
PacketRoster::PacketRoster(PacketRoster&&) noexcept = default;
PacketRoster& PacketRoster::operator=(PacketRoster&&) noexcept = default;

bool PacketRoster::install(const SymbolResolver& resolver, std::string_view profileJson, std::string& error) {
#ifdef _WIN32
    try {
        (void)resolver; // PacketAPI::installReceive does not need a resolver.
        impl_->rosterService.start(impl_->packetAPI);
        if (!impl_->packetAPI.installReceive(profileJson, error)) {
            impl_->rosterService.stop(impl_->packetAPI);
            return false;
        }
        impl_->installed = true;
        return true;
    } catch (...) {
        error = "exception in PacketRoster::install";
        return false;
    }
#else
    (void)resolver; (void)profileJson;
    error = "native PacketRoster unavailable on this host";
    return false;
#endif
}

bool PacketRoster::install(std::string_view selectedProfileJson, std::string_view embeddedProfileJson, std::string& error) {
#ifdef _WIN32
    try {
        impl_->rosterService.start(impl_->packetAPI);
        if (!impl_->packetAPI.installReceive(selectedProfileJson, embeddedProfileJson, error)) {
            impl_->rosterService.stop(impl_->packetAPI);
            return false;
        }
        impl_->installed = true;
        return true;
    } catch (...) {
        error = "exception in PacketRoster::install";
        return false;
    }
#else
    (void)selectedProfileJson; (void)embeddedProfileJson;
    error = "native PacketRoster unavailable on this host";
    return false;
#endif
}

std::shared_ptr<const std::vector<RosterPlayerEntry>> PacketRoster::snapshotShared() const noexcept {
#ifdef _WIN32
    if (!impl_->installed) return nullptr;
    try {
        const auto native = impl_->rosterService.snapshotShared();
        auto result = std::make_shared<std::vector<RosterPlayerEntry>>();
        if (native) {
            result->reserve(native->size());
            for (const auto& player : *native) {
                RosterPlayerEntry entry;
                entry.uuid = player.uuid;
                entry.uniqueId = player.uniqueId;
                entry.name = player.name;
                entry.face = player.face;
                entry.hasFace = player.hasFace;
                result->push_back(std::move(entry));
            }
        }
        return result;
    } catch (...) {
        return nullptr;
    }
#else
    return nullptr;
#endif
}

void PacketRoster::stop() noexcept {
#ifdef _WIN32
    if (impl_->installed) {
        impl_->packetAPI.stop();
        impl_->rosterService.stop(impl_->packetAPI);
        impl_->installed = false;
    }
#else
    (void)impl_;
#endif
}

} // namespace bedrock::sdk