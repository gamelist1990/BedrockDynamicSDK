#pragma once

#include "entity_health_cache.hpp"
#include "game_frame.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <bedrock/detail/hook_dispatch.hpp>
#include <bedrock/native/actor_components.hpp>
#include <bedrock/native/game_access.hpp>
#include <bedrock/native/safe_teleport.hpp>
#include <bedrock/packets/packet_api.hpp>
#include <bedrock/packets/protocol_codec.hpp>
#include <nlohmann/json.hpp>

namespace bedrock::sdk::detail {

struct NativeGameBindings {
    NativeGameAccess access;
    ::bedrock::detail::HookDispatch dispatch{access};
    NativeActorLayout actors;
    NativeActorComponents components;
    NativeWorld terrain;
    NativeSafeTeleport gui;
    bedrock::PacketAPI packets;
    bedrock::ProtocolCodec codec;
    std::optional<native_motion::Layout> motion;
    std::uint32_t playerType{};
    std::size_t typeOffset{};
    bool terrainReady{}, guiReady{}, packetReceiveReady{};
    // The game's own MobEffect add/remove functions and its default-instance
    // template, resolved from the exact-build profile in load(). Null when
    // the profile does not define them; effect writes then fall back to the
    // packet mirror only.
    void* effectAddFn{};
    void* effectRemoveFn{};
    const void* effectInstanceTemplate{};
    std::atomic<std::shared_ptr<WorldSession>> currentSession;

    // Item registry and live item actors are client-level state: the registry
    // packet arrives before the first level tick creates a WorldSession, and
    // item runtime IDs stay unique across world sessions. Keeping them here
    // means a packet is never dropped just because no frame exists yet.
    mutable std::mutex itemStateMutex;
    std::unordered_map<std::uint16_t, std::string> itemRuntimeIds;
    std::unordered_map<std::uint64_t, ItemStack> itemEntities;

    bool load(const RuntimeProfile& profile, std::string& error);
    bool installPacketReceive(const RuntimeProfile& profile, std::string& error);
    void observePacket(const bedrock::PacketReadEvent& event);
    void resetHealth();

private:
    friend class NativeGameFrame;
    // Actor spawn packets can arrive before the first WorldSession tick.
    mutable std::mutex healthMutex;
    EntityHealthCache entityHealth;

    void observeHealthPacket(bedrock::PacketId, std::string_view);
    void onUpdateAttributes(const nlohmann::json& payload);
    void onMobEffect(const nlohmann::json& payload);
    void onAddItemEntity(std::string_view payload);
    void onTakeItemEntity(const nlohmann::json& payload);
    void onItemRegistry(std::string_view payload);
    void onDimensionPacket(const nlohmann::json& payload, const char* field);
};

class NativeGameFrame final : public GameFrame {
public:
    NativeGameFrame(NativeGameBindings&, void* client, void* level, std::uint64_t epoch,
                    const std::shared_ptr<WorldSession>&);
    std::optional<std::uint64_t> localPlayerId() const override { return localId_; }
    const std::vector<std::uint64_t>& entityIds() override;
    bool contains(std::uint64_t) override;
    bool isPlayer(std::uint64_t) override;
    std::optional<std::string> typeId(std::uint64_t) override;
    std::optional<std::string> name(std::uint64_t) override;
    std::optional<Vector3> location(std::uint64_t) override;
    std::optional<Rotation> rotation(std::uint64_t) override;
    std::optional<std::string> dimensionTypeId() const override;
    std::optional<MovementComponent> movement(std::uint64_t) override;
    std::optional<std::string> blockTypeId(BlockPosition) override;
    std::optional<InventoryComponent> inventory(std::uint64_t) override;
    std::optional<EquippableComponent> equippable(std::uint64_t) override;
    std::optional<ItemStack> itemStack(std::uint64_t) const override;
    std::optional<HealthComponent> health(std::uint64_t) const override;
    std::optional<FoodComponent> food(std::uint64_t) const override;
    Result<void> sendMessage(std::uint64_t, std::string_view) override;
    Result<void> setBlockType(BlockPosition, std::string_view) override;
    Result<void> addEffect(std::uint64_t, std::string_view, double, EffectOptions) override;
    Result<bool> removeEffect(std::uint64_t, std::string_view) override;
    Result<std::vector<Effect>> effects(std::uint64_t) const override;
    Result<void> clearEffects(std::uint64_t) override;

private:
    void* resolve(std::uint64_t);
    std::optional<std::uint32_t> actorType(std::uint64_t);
    NativeGameBindings& bindings_;
    void* localActor_{};
    void* level_{};
    std::optional<std::uint64_t> localId_;
    NativeWorld::TickScope terrain_;
    NativeSafeTeleport::Scope gui_;
    bool enumerated_{};
    std::unordered_map<std::uint64_t, void*> handles_;
    std::vector<std::uint64_t> ids_;
};

} // namespace bedrock::sdk::detail
