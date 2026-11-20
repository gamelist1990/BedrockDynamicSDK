#pragma once

#include <bedrock/detail/actor_identity.hpp>
#include <bedrock/detail/hook_dispatch.hpp>

// Only collection.cpp sees this namespace. Keep its checked-in algorithm
// byte-for-byte unchanged without retaining an Actor wrapper in the SDK.
namespace automine::client::bedrock {

using ::bedrock::Vec3;
using ::bedrock::detail::ActorIdentity;

class PickupRead final {
public:
    PickupRead(const ::bedrock::detail::HookDispatch& dispatch, void* actor) noexcept
        : access_(dispatch.nativeAccess()), actor_(actor) {}

    ActorIdentity getIdentity() const {
        return ::bedrock::detail::readActorIdentity(access_, actor_);
    }

    std::optional<::bedrock::ItemStack> getMainHand() const {
        if (!actor_ || !access_.isActorValid(actor_)) return std::nullopt;
        auto item = access_.equipmentItem(actor_, ::bedrock::EquipmentSlot::MainHand);
        if (item && !item->valid()) item.reset();
        return item;
    }

private:
    const ::bedrock::ClientAccess& access_;
    void* actor_;
};

using Actor = PickupRead;

} // namespace automine::client::bedrock
