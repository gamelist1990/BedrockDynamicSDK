#include "bedrock/sdk/roster.hpp"
#include "resolver_access.hpp"
#ifdef _WIN32
#include <bedrock/native/game_access.hpp>
#include <bedrock/enums/game_type.hpp>
#endif
#include <algorithm>
#include <utility>

namespace bedrock::sdk {

struct RosterAccess::Impl {
#ifdef _WIN32
    bedrock::NativeGameAccess access;
#endif
};

RosterAccess::RosterAccess() : impl_(std::make_unique<Impl>()) {}
RosterAccess::~RosterAccess() = default;
RosterAccess::RosterAccess(RosterAccess&&) noexcept = default;
RosterAccess& RosterAccess::operator=(RosterAccess&&) noexcept = default;

bool RosterAccess::configure(const Profile& profile, const SymbolResolver& resolver, std::string& error) {
#ifdef _WIN32
    (void)resolver;
    auto runtime = ProfileRuntimeAccess::runtime(profile);
    if (!runtime) { error = "profile runtime unavailable"; return false; }
    return impl_->access.load(*runtime, error);
#else
    (void)profile; (void)resolver;
    error = "native RosterAccess unavailable on this host";
    return false;
#endif
}

void* RosterAccess::localPlayer() const noexcept {
#ifdef _WIN32
    return impl_->access.localPlayer();
#else
    return nullptr;
#endif
}

std::optional<std::uint64_t> RosterAccess::actorRuntimeId(void* actor) const noexcept {
#ifdef _WIN32
    return impl_->access.actorRuntimeId(actor);
#else
    (void)actor;
    return std::nullopt;
#endif
}

std::optional<GameType> RosterAccess::gameType(void* actor) const noexcept {
#ifdef _WIN32
    auto native = impl_->access.gameType(actor);
    if (!native) return std::nullopt;
    return static_cast<GameType>(static_cast<std::int32_t>(*native));
#else
    (void)actor;
    return std::nullopt;
#endif
}

std::vector<RosterEntry> RosterAccess::rosterSnapshot(void* world) const {
#ifdef _WIN32
    const auto native = impl_->access.rosterSnapshot(world);
    std::vector<RosterEntry> result;
    result.reserve(native.size());
    for (const auto& player : native) {
        RosterEntry entry;
        entry.name = player.name;
        entry.uuid = player.uuid;
        entry.uniqueId = player.uniqueId;
        entry.face = player.face;
        entry.hasFace = player.hasFace;
        result.push_back(std::move(entry));
    }
    return result;
#else
    (void)world;
    return {};
#endif
}

FaceResult RosterAccess::face(void* world, const std::array<std::byte, 16>& uuid) const {
#ifdef _WIN32
    FaceResult result;
    result.found = impl_->access.face(world, uuid, result.pixels);
    return result;
#else
    (void)world; (void)uuid;
    return {};
#endif
}

EquipmentSnapshot RosterAccess::equipmentSnapshot(void* actor, EquipmentSlot slot) const noexcept {
#ifdef _WIN32
    bedrock::EquipmentSlot nativeSlot = bedrock::EquipmentSlot::MainHand;
    switch (slot) {
    case EquipmentSlot::MainHand: nativeSlot = bedrock::EquipmentSlot::MainHand; break;
    case EquipmentSlot::Offhand: nativeSlot = bedrock::EquipmentSlot::Offhand; break;
    case EquipmentSlot::Head: nativeSlot = bedrock::EquipmentSlot::Head; break;
    case EquipmentSlot::Chest: nativeSlot = bedrock::EquipmentSlot::Chest; break;
    case EquipmentSlot::Legs: nativeSlot = bedrock::EquipmentSlot::Legs; break;
    case EquipmentSlot::Feet: nativeSlot = bedrock::EquipmentSlot::Feet; break;
    }
    const auto native = impl_->access.equipmentSnapshot(actor, nativeSlot);
    EquipmentSnapshot result;
    result.available = native.available;
    if (native.available && native.item) {
        result.hasItem = true;
        result.identifier = native.item->identifier;
        result.displayName = native.item->displayName;
        result.count = native.item->count;
    }
    return result;
#else
    (void)actor; (void)slot;
    return {};
#endif
}

bool RosterAccess::packetRosterSupported() const noexcept {
#ifdef _WIN32
    return impl_->access.packetRosterSupported();
#else
    return false;
#endif
}

bool RosterAccess::displayNamesAvailable() const noexcept {
#ifdef _WIN32
    return impl_->access.displayNamesAvailable();
#else
    return false;
#endif
}

void RosterAccess::setDisplayNamesEnabled(bool enabled) noexcept {
#ifdef _WIN32
    impl_->access.setDisplayNamesEnabled(enabled);
#else
    (void)enabled;
#endif
}

void RosterAccess::clearWorldCaches() noexcept {
#ifdef _WIN32
    impl_->access.clearWorldCaches();
#else
    (void)impl_;
#endif
}

std::vector<void*> RosterAccess::actorHandles(void* world) const noexcept {
#ifdef _WIN32
    return impl_->access.actorHandles(world);
#else
    (void)world;
    return {};
#endif
}

void RosterAccess::setRosterProvider(std::function<std::shared_ptr<const std::vector<RosterEntry>>()> provider) {
#ifdef _WIN32
    impl_->access.setRosterProvider([provider = std::move(provider)]() -> std::shared_ptr<const std::vector<bedrock::PacketPlayer>> {
        const auto entries = provider ? provider() : nullptr;
        if (!entries) return nullptr;
        auto result = std::make_shared<std::vector<bedrock::PacketPlayer>>();
        result->reserve(entries->size());
        for (const auto& e : *entries) {
            bedrock::PacketPlayer player;
            player.uuid = e.uuid;
            player.uniqueId = e.uniqueId;
            player.name = e.name;
            player.face = e.face;
            player.hasFace = e.hasFace;
            result->push_back(std::move(player));
        }
        return result;
    });
#else
    (void)provider;
#endif
}

void RosterAccess::setNameProvider(std::function<std::optional<std::string>(void*)> provider) {
#ifdef _WIN32
    impl_->access.setNameProvider(std::move(provider));
#else
    (void)provider;
#endif
}

} // namespace bedrock::sdk