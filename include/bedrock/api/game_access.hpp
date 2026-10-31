#pragma once
#include <bedrock/api/geometry.hpp>
#include <bedrock/api/item_stack.hpp>
#include <bedrock/api/server_address.hpp>
#include <bedrock/api/blocks.hpp>
#include <bedrock/enums/actors.hpp>
#include <bedrock/enums/input.hpp>
#include <optional>
#include <vector>
namespace bedrock {
// Implemented by the game-specific adapter after it has validated the live
// pointers/layout for the exact executable. All returned pointers are borrowed.
class GameAccess : public BlockAccess {
  public:
    virtual ~GameAccess() = default;
    virtual void observeClient(void*) noexcept {}
    virtual bool beginWorldTick(void*) noexcept { return true; }
    virtual void endWorldTick() noexcept {}
    [[nodiscard]] virtual bool isActorValid(void *actor) const noexcept = 0;
    [[nodiscard]] virtual void *localPlayer() const noexcept = 0;
    [[nodiscard]] virtual void *worldOf(void *actor) const noexcept = 0;
    [[nodiscard]] virtual std::optional<std::string> nameTag(void *actor) const = 0;
    // Optional native world source. Implementations return borrowed Actor
    // handles from the live world's entity registry, never a temporary Actor.
    [[nodiscard]] virtual std::vector<void *> actorHandles(void *world) const {
        (void)world;
        return {};
    }
    [[nodiscard]] virtual std::optional<ActorBounds> actorBounds(void *actor) const {
        (void)actor;
        return std::nullopt;
    }
    [[nodiscard]] virtual std::optional<Vec3> actorPosition(void *actor) const {
        (void)actor;
        return std::nullopt;
    }
    [[nodiscard]] virtual std::optional<ActorRotation> actorRotation(void *actor) const {
        (void)actor;
        return std::nullopt;
    }
    [[nodiscard]] virtual std::optional<std::uint32_t> actorEntityId(void *actor) const {
        (void)actor;
        return std::nullopt;
    }
    // Bedrock's long-lived ActorUniqueID component, distinct from both the
    // registry entity handle and the network RuntimeID.
    [[nodiscard]] virtual std::optional<std::uint64_t> actorUniqueId(void *actor) const {
        (void)actor;
        return std::nullopt;
    }
    // Bedrock ActorRuntimeID from RuntimeIDComponent. This is distinct from
    // the registry's raw EnTT entity ID exposed by actorEntityId().
    [[nodiscard]] virtual std::optional<std::uint64_t> actorRuntimeId(void *actor) const {
        (void)actor;
        return std::nullopt;
    }
    // Return only this Actor's currently held hand item. Do not enumerate or
    // expose the Actor's full inventory; local-player inventory access is a
    // separate API with a different lifetime and access path.
    [[nodiscard]] virtual std::optional<HeldItemInfo> heldItem(void *actor) const {
        (void)actor;
        return std::nullopt;
    }
    [[nodiscard]] virtual std::optional<ItemStack> equipmentItem(void *actor,
                                                                 EquipmentSlot slot) const {
        return slot == EquipmentSlot::MainHand ? heldItem(actor) : std::nullopt;
    }
    // Inventory access is exposed only through LocalPlayer. Implementations
    // must validate the supplied handle against the live local player before
    // reading any inventory pointer.
    [[nodiscard]] virtual std::size_t localInventorySize(void *localPlayer) const {
        (void)localPlayer;
        return 0;
    }
    [[nodiscard]] virtual std::optional<ItemStack> localInventoryItem(void *localPlayer,
                                                                      std::size_t slot) const {
        (void)localPlayer;
        (void)slot;
        return std::nullopt;
    }
    [[nodiscard]] virtual std::optional<std::size_t> selectedHotbarSlot(void *localPlayer) const {
        (void)localPlayer;
        return std::nullopt;
    }
    [[nodiscard]] virtual std::vector<std::string> playerListNames(void *world) const = 0;
    [[nodiscard]] virtual std::optional<ServerAddress> serverAddress(void *localPlayer) const = 0;

    // Input methods return false when the adapter cannot perform that action.
    // No Win32 input injection is used by these wrappers.
    [[nodiscard]] virtual bool keyDown(unsigned key) const noexcept {
        (void)key;
        return false;
    }
    virtual bool sendKey(unsigned key, bool pressed) noexcept {
        (void)key;
        (void)pressed;
        return false;
    }
    [[nodiscard]] virtual bool mouseDown(MouseButton button) const noexcept {
        (void)button;
        return false;
    }
    virtual bool click(MouseButton button) noexcept {
        (void)button;
        return false;
    }
    virtual bool moveMouse(int deltaX, int deltaY) noexcept {
        (void)deltaX;
        (void)deltaY;
        return false;
    }
    virtual bool turn(float yawDelta, float pitchDelta) noexcept {
        (void)yawDelta;
        (void)pitchDelta;
        return false;
    }
};

// Useful for input-only examples that do not need to provide version-specific
// Actor/world access. The hook-backed keyboard and mouse APIs still work.
class EmptyGameAccess final : public GameAccess {
  public:
    [[nodiscard]] bool isActorValid(void *) const noexcept override {
        return false;
    }
    [[nodiscard]] void *localPlayer() const noexcept override {
        return nullptr;
    }
    [[nodiscard]] void *worldOf(void *) const noexcept override {
        return nullptr;
    }
    [[nodiscard]] std::optional<std::string> nameTag(void *) const override {
        return std::nullopt;
    }
    [[nodiscard]] std::vector<std::string> playerListNames(void *) const override {
        return {};
    }
    [[nodiscard]] std::optional<ServerAddress> serverAddress(void *) const override {
        return std::nullopt;
    }
};
inline EmptyGameAccess &emptyGameAccess() {
    static EmptyGameAccess access;
    return access;
}

} // namespace bedrock
