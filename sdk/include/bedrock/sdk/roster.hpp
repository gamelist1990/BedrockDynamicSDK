#pragma once
#include <bedrock/sdk/game_hooks.hpp>
#include <bedrock/sdk/profiles.hpp>
#include <bedrock/sdk/resolver.hpp>
#include <bedrock/sdk/game_types.hpp>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <array>

namespace bedrock { class NativeGameAccess; }

namespace bedrock::sdk {

class RosterAccess;
namespace detail { bedrock::NativeGameAccess* nativeGameAccess(RosterAccess&) noexcept; }

// ------------------------------------------------------------------------
// RosterEntry — value-type snapshot of a single roster player.
// Mirrors the subset of PacketPlayer needed for TabList, without native
// pointers or version-dependent wire fields.
// ------------------------------------------------------------------------
struct RosterEntry {
    std::string name;
    std::array<std::byte, 16> uuid{};
    std::int64_t uniqueId{};
    std::array<std::uint32_t, 64> face{};
    bool hasFace{};
};

// FaceResult — the native face() returns a bool (face known) alongside the
// pixels; the bool must survive the facade (a failed lookup must not be
// misread as "all-black face" by scanning the pixels).
struct FaceResult {
    std::array<std::uint32_t, 64> pixels{};
    bool found{};
};

// EquipmentSnapshot — value-type equipment snapshot, mirrors the subset
// of NativeGameAccess::EquipmentSnapshot used by TabList.
struct EquipmentSnapshot {
    bool available{};
    bool hasItem{};
    std::string identifier;
    std::string displayName;
    unsigned count{};
};

// ItemStackValue — owned value snapshot of one item stack, mirroring the
// native ItemStack fields and its valid()/getName() semantics.
struct ItemStackValue {
    std::string identifier;
    std::string displayName;
    unsigned count{};
    std::string customName;
    std::string translationKey;

    [[nodiscard]] bool valid() const noexcept {
        return !identifier.empty() || !displayName.empty() || !customName.empty() ||
               !translationKey.empty();
    }
    [[nodiscard]] std::string getName() const {
        if (!customName.empty())
            return customName;
        if (!displayName.empty())
            return displayName;
        return identifier;
    }
};

// ActorIdentityValue — value-type actor identity, mirroring the native
// detail::ActorIdentity read (name tag / name / player flag).
struct ActorIdentityValue {
    std::string nameTag;
    std::string name;
    bool isPlayer{};
};

// RosterAccess — opaque impl wraps NativeGameAccess; value-semantic API
// for the TabList-facing subset.
class RosterAccess final {
public:
    RosterAccess();
    ~RosterAccess();

    // Delete copy/mopy, enable move.
    RosterAccess(const RosterAccess&) = delete;
    RosterAccess& operator=(const RosterAccess&) = delete;
    RosterAccess(RosterAccess&&) noexcept;
    RosterAccess& operator=(RosterAccess&&) noexcept;

    // Initialise from a Profile + SymbolResolver (configures native adapter
    // behind the opaque Impl, exactly like ActorFacade::configure).
    bool configure(const Profile& profile, const SymbolResolver& resolver, std::string& error);

    // --- Value entries -------------------------------------------------------
    [[nodiscard]] void* localPlayer() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> actorRuntimeId(void* actor) const noexcept;
    [[nodiscard]] std::optional<GameType> gameType(void* actor) const noexcept;

    // Roster snapshot — value-type entries only. Mirrors
    // NativeGameAccess::rosterSnapshot() but returns owned value entries.
    [[nodiscard]] std::vector<RosterEntry> rosterSnapshot(void* world) const;

    // Face bytes — mirrors NativeGameAccess::face(). Returns the 64 RGBA
    // pixels plus whether a face is known for the uuid (found=false when
    // the player has no face on record; pixels are then all zero).
    [[nodiscard]] FaceResult face(void* world, const std::array<std::byte, 16>& uuid) const;

    // Equipment snapshot — mirrors NativeGameAccess::equipmentSnapshot()
    // but returns a value-type EquipmentSnapshot. The slot is honoured
    // (MainHand by default for TabList); it is not ignored.
    [[nodiscard]] EquipmentSnapshot equipmentSnapshot(void* actor, EquipmentSlot slot) const noexcept;

    // --- Item / identity reads (AutoMine-facing subset) ---------------------
    // These read through the same NativeGameAccess and obey its LevelTick
    // scope: the access must be driven by a tick (see Hooks::useGameAccess)
    // for the reads to see the world.
    [[nodiscard]] std::optional<ItemStackValue> equipmentItem(void* actor, EquipmentSlot slot) const;
    [[nodiscard]] std::optional<ItemStackValue> localInventoryItem(void* player, std::size_t index) const;
    [[nodiscard]] std::size_t localInventorySize(void* player) const;
    [[nodiscard]] bool isActorValid(void* actor) const noexcept;
    [[nodiscard]] ActorIdentityValue actorIdentity(void* actor) const;

    // --- Provider / flag callbacks -------------------------------------------
    bool packetRosterSupported() const noexcept;
    bool displayNamesAvailable() const noexcept;
    void setDisplayNamesEnabled(bool) noexcept;
    void clearWorldCaches() noexcept;
    void setRosterProvider(std::function<std::shared_ptr<const std::vector<RosterEntry>>()> provider);
    void setNameProvider(std::function<std::optional<std::string>(void*)> provider);
    [[nodiscard]] std::vector<void*> actorHandles(void* world) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend bedrock::NativeGameAccess* detail::nativeGameAccess(RosterAccess&) noexcept;
};

} // namespace bedrock::sdk