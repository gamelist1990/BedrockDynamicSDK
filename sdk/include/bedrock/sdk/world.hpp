#pragma once

#include "game_types.hpp"
#include "result.hpp"

#include <array>
#include <memory>
#include <string_view>
#include <type_traits>

namespace bedrock::sdk {
namespace detail {
struct WorldSession;
class GameFrame;
}

class Entity;
class Player;

class Block {
public:
    [[nodiscard]] const std::string& typeId() const noexcept { return typeId_; }
    [[nodiscard]] BlockPosition location() const noexcept { return position_; }
    [[nodiscard]] Result<void> setType(std::string_view typeId) const;
private:
    friend class Dimension;
    Block(std::weak_ptr<detail::WorldSession> session, std::uint64_t dimension,
          BlockPosition position, std::string typeId)
        : session_(std::move(session)), dimension_(dimension), position_(position), typeId_(std::move(typeId)) {}
    std::weak_ptr<detail::WorldSession> session_;
    std::uint64_t dimension_;
    BlockPosition position_;
    std::string typeId_;
};

class Dimension {
public:
    [[nodiscard]] const std::optional<std::string>& typeId() const noexcept { return typeId_; }
    [[nodiscard]] std::optional<Block> getBlock(BlockPosition position) const;
    [[nodiscard]] std::vector<Entity> getEntities() const;
    [[nodiscard]] Result<Entity> spawnEntity(std::string_view typeId, Vector3 position) const;
private:
    friend class World;
    friend class Entity;
    Dimension(std::weak_ptr<detail::WorldSession> session, std::uint64_t revision,
              std::optional<std::string> typeId)
        : session_(std::move(session)), revision_(revision), typeId_(std::move(typeId)) {}
    std::weak_ptr<detail::WorldSession> session_;
    std::uint64_t revision_;
    std::optional<std::string> typeId_;
};

class EquippableComponent {
public:
    [[nodiscard]] std::optional<ItemStack> getEquipment(EquipmentSlot slot) const;
private:
    friend class detail::GameFrame;
    explicit EquippableComponent(std::array<std::optional<ItemStack>, 6> slots)
        : slots_(std::move(slots)) {}
    std::array<std::optional<ItemStack>, 6> slots_;
};

class Entity {
public:
    [[nodiscard]] bool isValid() const;
    [[nodiscard]] std::optional<std::string> typeId() const;
    [[nodiscard]] std::optional<Vector3> location() const;
    [[nodiscard]] std::optional<Rotation> getRotation() const;
    [[nodiscard]] std::optional<Dimension> dimension() const;
    [[nodiscard]] std::optional<ItemStack> getItemStack() const;
    [[nodiscard]] Result<void> teleport(Vector3 position) const;
    [[nodiscard]] Result<bool> addTag(std::string_view tag) const;
    [[nodiscard]] std::optional<DynamicProperty> getDynamicProperty(std::string_view key) const;
    [[nodiscard]] Result<void> setDynamicProperty(std::string_view key,
                                                  std::optional<DynamicProperty> value) const;

    template<class T>
    [[nodiscard]] std::optional<T> getComponent() const {
        if constexpr (std::is_same_v<T, HealthComponent>) return health();
        else if constexpr (std::is_same_v<T, FoodComponent>) return food();
        else if constexpr (std::is_same_v<T, MovementComponent>) return movement();
        else if constexpr (std::is_same_v<T, InventoryComponent>) return inventory();
        else if constexpr (std::is_same_v<T, EquippableComponent>) return equippable();
        else static_assert(!std::is_same_v<T, T>, "Unsupported game component type");
    }

protected:
    Entity(std::weak_ptr<detail::WorldSession> session, std::uint64_t id)
        : session_(std::move(session)), id_(id) {}
private:
    friend class World;
    friend class Dimension;
    std::optional<HealthComponent> health() const;
    std::optional<FoodComponent> food() const;
    std::optional<MovementComponent> movement() const;
    std::optional<InventoryComponent> inventory() const;
    std::optional<EquippableComponent> equippable() const;
    friend class Player;
    std::weak_ptr<detail::WorldSession> session_;
    std::uint64_t id_;
};

class Player final : public Entity {
public:
    [[nodiscard]] std::optional<std::string> name() const;
    [[nodiscard]] Result<void> sendMessage(std::string_view text) const;
    [[nodiscard]] Result<void> addEffect(std::string_view typeId, double duration,
                                         EffectOptions options = {}) const;
    [[nodiscard]] Result<bool> removeEffect(std::string_view typeId) const;
    [[nodiscard]] Result<std::vector<Effect>> getEffects() const;
    [[nodiscard]] Result<void> clearEffects() const;
private:
    friend class World;
    using Entity::Entity;
};

class World {
public:
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    [[nodiscard]] std::optional<Player> localPlayer() const;
    [[nodiscard]] std::vector<Player> getAllPlayers() const;
    [[nodiscard]] std::vector<Player> getPlayers() const { return getAllPlayers(); }
    [[nodiscard]] std::optional<Dimension> currentDimension() const;
    [[nodiscard]] std::optional<Dimension> getDimension(std::string_view typeId) const;
    [[nodiscard]] Result<void> sendMessage(std::string_view text) const;
    [[nodiscard]] std::optional<DynamicProperty> getDynamicProperty(std::string_view key) const;
    [[nodiscard]] Result<void> setDynamicProperty(std::string_view key,
                                                  std::optional<DynamicProperty> value) const;
    // Resolve from the game frame that is active on the current tick instead of
    // from this Mod-bound World instance. A host that dispatches commands on a
    // shared tick (ScriptClient) may run a JS handler while the active frame is
    // bound to a different Mod's World; the instance methods above intentionally
    // return nothing in that case (see world_test.cpp). These session-based
    // resolvers let such a host stay live in-session without weakening the
    // per-Mod World binding. They return empty/error when no frame is active.
    [[nodiscard]] static std::optional<Player> activeLocalPlayer();
    [[nodiscard]] static std::vector<Player> activePlayers();
    [[nodiscard]] static std::optional<Dimension> activeDimension();
    [[nodiscard]] static std::optional<DynamicProperty> activeWorldProperty(std::string_view key);
    [[nodiscard]] static Result<void> setActiveWorldProperty(std::string_view key,
                                                             std::optional<DynamicProperty> value);
private:
    friend class Mod;
    World() = default;
};

} // namespace bedrock::sdk
