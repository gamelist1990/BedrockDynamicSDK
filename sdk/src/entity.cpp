#include "game_frame.hpp"

#include <bedrock/sdk/client_info.hpp>

#include <cmath>

namespace bedrock::sdk {
namespace {
detail::GameFrame* entityFrame(const std::weak_ptr<detail::WorldSession>& session, std::uint64_t id) {
    auto* frame = detail::frameFor(session);
    return frame && frame->contains(id) ? frame : nullptr;
}

detail::GameFrame* localPlayerFrame(const std::weak_ptr<detail::WorldSession>& session, std::uint64_t id) {
    auto* frame = detail::frameFor(session);
    return frame && frame->localPlayerId() == id && frame->contains(id) ? frame : nullptr;
}
}

std::optional<ItemStack> EquippableComponent::getEquipment(EquipmentSlot slot) const {
    const auto index = static_cast<std::size_t>(slot);
    return index < slots_.size() ? slots_[index] : std::nullopt;
}

bool Entity::isValid() const { return entityFrame(session_, id_) != nullptr; }

std::optional<std::string> Entity::typeId() const {
    if (auto* frame = entityFrame(session_, id_)) return frame->typeId(id_);
    return std::nullopt;
}

std::optional<Vector3> Entity::location() const {
    if (auto* frame = entityFrame(session_, id_)) return frame->location(id_);
    return std::nullopt;
}

std::optional<Rotation> Entity::getRotation() const {
    if (auto* frame = entityFrame(session_, id_)) return frame->rotation(id_);
    return std::nullopt;
}

std::optional<Dimension> Entity::dimension() const {
    if (auto* frame = entityFrame(session_, id_))
        return Dimension{session_, frame->dimensionRevision, frame->dimensionTypeId()};
    return std::nullopt;
}

std::optional<ItemStack> Entity::getItemStack() const {
    if (auto* frame = entityFrame(session_, id_)) return frame->itemStack(id_);
    return std::nullopt;
}

Result<void> Entity::teleport(Vector3 position) const {
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
        return Result<void>::error(ErrorCode::Invalid, "Teleport coordinates must be finite");
    if (auto* frame = entityFrame(session_, id_)) return frame->teleport(id_, position);
    return detail::unavailable("Entity is not loaded");
}

Result<bool> Entity::addTag(std::string_view tag) const {
    if (auto* frame = entityFrame(session_, id_)) return frame->addTag(id_, tag);
    return detail::unavailable<bool>("Entity is not loaded");
}

std::optional<DynamicProperty> Entity::getDynamicProperty(std::string_view key) const {
    if (const auto* frame = entityFrame(session_, id_)) {
        const auto& all = frame->session->entityProperties;
        if (const auto it = all.find(id_); it != all.end()) return detail::getProperty(it->second, key);
    }
    return std::nullopt;
}

Result<void> Entity::setDynamicProperty(std::string_view key, std::optional<DynamicProperty> value) const {
    if (auto* frame = entityFrame(session_, id_)) {
        auto& all = frame->session->entityProperties;
        if (!value && !all.contains(id_) && !key.empty()) return Result<void>::ok();
        auto& properties = all[id_];
        auto result = detail::setProperty(properties, key, std::move(value));
        if (properties.empty()) all.erase(id_);
        return result;
    }
    return detail::unavailable("Entity is not loaded");
}

std::optional<HealthComponent> Entity::health() const {
    if (auto* frame = entityFrame(session_, id_)) return frame->health(id_);
    return std::nullopt;
}

std::optional<FoodComponent> Entity::food() const {
    if (auto* frame = entityFrame(session_, id_)) return frame->food(id_);
    return std::nullopt;
}

std::optional<MovementComponent> Entity::movement() const {
    if (auto* frame = entityFrame(session_, id_)) return frame->movement(id_);
    return std::nullopt;
}

std::optional<InventoryComponent> Entity::inventory() const {
    if (auto* frame = entityFrame(session_, id_)) return frame->inventory(id_);
    return std::nullopt;
}

std::optional<EquippableComponent> Entity::equippable() const {
    if (auto* frame = entityFrame(session_, id_)) return frame->equippable(id_);
    return std::nullopt;
}

std::optional<std::string> Player::name() const {
    if (auto* frame = entityFrame(session_, id_)) return frame->name(id_);
    return std::nullopt;
}

Result<void> Player::sendMessage(std::string_view text) const {
    if (auto* frame = entityFrame(session_, id_)) return frame->sendMessage(id_, text);
    return detail::unavailable("Player is not loaded");
}

Result<void> Player::addEffect(std::string_view typeId, double duration, EffectOptions options) const {
    if (typeId.empty() || !(duration > 0))
        return Result<void>::error(ErrorCode::Invalid, "An effect ID and positive tick duration are required");
    if (auto* frame = localPlayerFrame(session_, id_)) return frame->addEffect(id_, typeId, duration, options);
    return detail::unavailable("Effect operations require the local player in the owning tick");
}

Result<bool> Player::removeEffect(std::string_view typeId) const {
    if (typeId.empty()) return Result<bool>::error(ErrorCode::Invalid, "An effect ID is required");
    if (auto* frame = localPlayerFrame(session_, id_)) return frame->removeEffect(id_, typeId);
    return detail::unavailable<bool>("Effect operations require the local player in the owning tick");
}

Result<std::vector<Effect>> Player::getEffects() const {
    if (auto* frame = localPlayerFrame(session_, id_)) return frame->effects(id_);
    return detail::unavailable<std::vector<Effect>>("Effect operations require the local player in the owning tick");
}

Result<void> Player::clearEffects() const {
    if (auto* frame = localPlayerFrame(session_, id_)) return frame->clearEffects(id_);
    return detail::unavailable("Effect operations require the local player in the owning tick");
}

namespace {
bool isLocalPlayer(const std::weak_ptr<detail::WorldSession>& session, std::uint64_t id) {
    auto* frame = detail::frameFor(session);
    return frame && frame->localPlayerId() == id;
}
}

double Player::fps() const {
    if (!isLocalPlayer(session_, id_)) return 0.0;
    return ClientInfo::fps();
}

PingSnapshot Player::ping() const {
    if (!isLocalPlayer(session_, id_)) return {};
    return ClientInfo::ping();
}

CpsSnapshot Player::cps() const {
    if (!isLocalPlayer(session_, id_)) return {};
    return ClientInfo::cps();
}

DeviceInfo Player::device() const {
    if (!isLocalPlayer(session_, id_)) return {};
    return ClientInfo::device();
}
} // namespace bedrock::sdk
