#pragma once

#include <bedrock/sdk.hpp>
#include <optional>
#include <string>

// Only collection.cpp sees this namespace. Keep its checked-in algorithm
// byte-for-byte unchanged without retaining an Actor wrapper in the SDK.
// The reads now go through the public RosterAccess bound to the mod's
// Hooks (Hooks::useGameAccess), which is the facade equivalent of the
// native HookDispatch's game access the shim used before.
namespace automine::client::bedrock {

using Vec3 = ::bedrock::sdk::Vec3Value;

struct ActorIdentity {
    std::string nameTag;
    std::string name;
    bool isPlayer{};
};

class PickupRead final {
public:
    PickupRead(::bedrock::sdk::Hooks& hooks, void* actor) noexcept
        : access_(hooks.gameAccess()), actor_(actor) {}

    ActorIdentity getIdentity() const {
        ActorIdentity out;
        if (access_) {
            const auto ident = access_->actorIdentity(actor_);
            out.nameTag = ident.nameTag;
            out.name = ident.name;
            out.isPlayer = ident.isPlayer;
        }
        return out;
    }

    std::optional<::bedrock::sdk::ItemStackValue> getMainHand() const {
        if (!actor_ || !access_ || !access_->isActorValid(actor_)) return std::nullopt;
        auto item = access_->equipmentItem(actor_, ::bedrock::sdk::EquipmentSlot::MainHand);
        if (item && !item->valid()) item.reset();
        return item;
    }

private:
    ::bedrock::sdk::RosterAccess* access_{};
    void* actor_{};
};

using Actor = PickupRead;

} // namespace automine::client::bedrock
