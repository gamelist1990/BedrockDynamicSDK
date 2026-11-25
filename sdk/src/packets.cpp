#include "bedrock/sdk/packets.hpp"
#include "resolver_access.hpp"
#ifdef _WIN32
#include <bedrock/packets/packet_api.hpp>
#include <bedrock/packets/player_roster_service.hpp>
#endif
#include <atomic>
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

RosterStatistics PacketRoster::statistics() const noexcept {
    RosterStatistics out;
#ifdef _WIN32
    if (!impl_->installed) return out;
    try {
        const auto s = impl_->rosterService.statistics();
        out.packets = s.packets;
        out.added = s.added;
        out.removed = s.removed;
        out.skinUpdates = s.skinUpdates;
        out.rejected = s.rejected;
        out.generation = s.generation;
        out.players = s.players;
        out.error = s.error;
        out.dropped = impl_->rosterService.dropped();
        const auto r = impl_->packetAPI.receiveStatistics();
        out.receiveCalls = r.calls;
        out.receiveFiltered = r.filtered;
        out.receiveCopied = r.copied;
        out.receiveStreamFailures = r.streamFailures;
        out.receiveDecodeFailures = r.decodeFailures;
        out.receivePublished = r.published;
        out.receiveNested = r.nested;
    } catch (...) {}
#endif
    return out;
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

// ---------------------------------------------------------------------------
// PacketObserver
// ---------------------------------------------------------------------------
struct PacketObserver::Impl {
#ifdef _WIN32
    bedrock::PacketAPI packetAPI{};
#endif
    bool installed{};
    Handler handler{};
    void* user{};
    std::atomic<std::uint64_t> delivered{};
};

PacketObserver::PacketObserver() : impl_(std::make_unique<Impl>()) {}
PacketObserver::~PacketObserver() = default;
PacketObserver::PacketObserver(PacketObserver&&) noexcept = default;
PacketObserver& PacketObserver::operator=(PacketObserver&&) noexcept = default;

bool PacketObserver::setHandler(Handler handler, void* user) noexcept {
    if (!handler || impl_->installed) return false;
    impl_->handler = handler;
    impl_->user = user;
    return true;
}

bool PacketObserver::install(const SymbolResolver& resolver, std::string_view profileJson,
                             std::string& error) {
#ifdef _WIN32
    try {
        (void)resolver; // PacketAPI::installReceive needs no resolver.
        if (impl_->installed) return true;
        if (!impl_->handler) {
            error = "PacketObserver::setHandler must be called before install";
            return false;
        }
        impl_->packetAPI.onAnyReceive([this](const bedrock::PacketReadEvent& event) {
            // Networking thread, after a successful decode. Scalars only.
            if (!impl_->handler) return;
            PacketObservation observation;
            observation.rawId = static_cast<int>(event.rawId());
            observation.bytes = event.payload().size();
            observation.serial = impl_->delivered.fetch_add(1, std::memory_order_relaxed) + 1;
            impl_->handler(observation, impl_->user);
        });
        if (!impl_->packetAPI.installReceive(profileJson, error)) return false;
        impl_->installed = true;
        return true;
    } catch (...) {
        error = "exception in PacketObserver::install";
        return false;
    }
#else
    (void)resolver; (void)profileJson;
    error = "native PacketObserver unavailable on this host";
    return false;
#endif
}

bool PacketObserver::install(std::string_view selectedProfileJson,
                             std::string_view embeddedProfileJson, std::string& error) {
#ifdef _WIN32
    try {
        if (impl_->installed) return true;
        if (!impl_->handler) {
            error = "PacketObserver::setHandler must be called before install";
            return false;
        }
        impl_->packetAPI.onAnyReceive([this](const bedrock::PacketReadEvent& event) {
            if (!impl_->handler) return;
            PacketObservation observation;
            observation.rawId = static_cast<int>(event.rawId());
            observation.bytes = event.payload().size();
            observation.serial = impl_->delivered.fetch_add(1, std::memory_order_relaxed) + 1;
            impl_->handler(observation, impl_->user);
        });
        if (!impl_->packetAPI.installReceive(selectedProfileJson, embeddedProfileJson, error))
            return false;
        impl_->installed = true;
        return true;
    } catch (...) {
        error = "exception in PacketObserver::install";
        return false;
    }
#else
    (void)selectedProfileJson; (void)embeddedProfileJson;
    error = "native PacketObserver unavailable on this host";
    return false;
#endif
}

bool PacketObserver::installed() const noexcept {
#ifdef _WIN32
    return impl_->installed;
#else
    return false;
#endif
}

PacketObservationStatistics PacketObserver::statistics() const noexcept {
    PacketObservationStatistics out;
#ifdef _WIN32
    try {
        const auto r = impl_->packetAPI.receiveStatistics();
        out.receiveCalls = r.calls;
        out.receiveCopied = r.copied;
        out.receiveFiltered = r.filtered;
        out.streamFailures = r.streamFailures;
        out.decodeFailures = r.decodeFailures;
        out.published = r.published;
        out.delivered = impl_->delivered.load(std::memory_order_relaxed);
    } catch (...) {}
#endif
    return out;
}

void PacketObserver::stop() noexcept {
#ifdef _WIN32
    if (impl_->installed) {
        impl_->packetAPI.stop();
        impl_->installed = false;
    }
#endif
    impl_->handler = nullptr;
    impl_->user = nullptr;
}

} // namespace bedrock::sdk