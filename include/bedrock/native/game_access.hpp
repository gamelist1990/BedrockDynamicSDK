#pragma once
#include <bedrock/api/game_access.hpp>
#include <bedrock/packets/player_list_packets.hpp>
#include <bedrock/profiles/profile_manager.hpp>
#include <bedrock/enums/game_type.hpp>
#include <functional>
#include <memory>

namespace bedrock {
// Owned value: distinguishes a confirmed empty slot from a failed native read.
struct EquipmentSnapshot {
    bool available{};
    std::optional<ItemStack> item;
};
// Exact-profile client access. Native pointers, STL/EnTT layouts, item calls and
// caches live in the SDK implementation. Native reads are limited to LevelTick.
class NativeGameAccess final : public GameAccess {
  public:
    using RosterProvider = std::function<std::shared_ptr<const std::vector<PacketPlayer>>() >;
    using NameProvider = std::function<std::optional<std::string>(void*)>;
    NativeGameAccess();
    ~NativeGameAccess();
    NativeGameAccess(const NativeGameAccess&) = delete;
    NativeGameAccess& operator=(const NativeGameAccess&) = delete;
    bool load(const RuntimeProfile&, std::string& error);
    void setRosterProvider(RosterProvider);
    void setNameProvider(NameProvider);
    bool packetRosterSupported() const noexcept;
    bool displayNamesAvailable() const noexcept;
    void setDisplayNamesEnabled(bool) noexcept;
    void clearWorldCaches() noexcept;
    std::optional<GameType> gameType(void*) const noexcept;
    EquipmentSnapshot equipmentSnapshot(void*, EquipmentSlot) const;
    bool face(void* world, const std::array<std::byte,16>& uuid,
              std::array<std::uint32_t,64>& pixels) const;
    std::vector<PacketPlayer> rosterSnapshot(void* world) const;
    void observeClient(void*) noexcept override;
    bool beginWorldTick(void*) noexcept override;
    void endWorldTick() noexcept override;
    bool isActorValid(void*) const noexcept override;
    void* localPlayer() const noexcept override;
    void* worldOf(void*) const noexcept override;
    std::vector<void*> actorHandles(void*) const override;
    std::optional<std::uint64_t> actorRuntimeId(void*) const override;
    std::optional<std::string> nameTag(void*) const override;
    std::optional<ItemStack> equipmentItem(void*, EquipmentSlot) const override;
    std::size_t localInventorySize(void*) const override;
    std::optional<ItemStack> localInventoryItem(void*, std::size_t) const override;
    std::optional<std::size_t> selectedHotbarSlot(void*) const override;
    std::vector<std::string> playerListNames(void*) const override;
    std::optional<ServerAddress> serverAddress(void*) const override { return std::nullopt; }
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace bedrock
