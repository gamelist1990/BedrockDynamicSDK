#pragma once
// Owned block snapshots. Safe to process on workers; stateIdentity is an opaque,
// world-scoped comparison token, never a dereferenceable pointer.
#include <array>
#include <compare>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
namespace bedrock {
struct BlockPos {
    int x{}, y{}, z{};
    auto operator<=>(const BlockPos&) const = default;
};
enum class CellKind { unknown, air, block };
enum class Face { down, up, north, south, west, east };
enum class Hand { main };
using StateValue=std::variant<int,std::string>;
struct BlockProperties {std::map<std::string,StateValue> values;};
struct PlacementRecipe {
    std::string itemIdentifier;
    Face face = Face::up;
    float yaw{}, pitch{};
    float hitX = .5f, hitY = .5f, hitZ = .5f;
    bool deferredState = false;
    bool augment = false;
    bool orientationKnown = false;
    bool exactStateSupported = false;
    bool operator==(const PlacementRecipe&) const = default;
};
struct Cell {
    BlockPos position;
    std::string identifier;
    std::optional<std::uint64_t> stateIdentity;
    CellKind kind = CellKind::unknown;
    std::shared_ptr<const BlockProperties> properties; // owned immutable copies, never native pointers
    bool supportsPlacement = false;
    bool nativePlacementCandidate = false; // Adapter must resolve and recheck the exact state before dispatch.
    std::optional<PlacementRecipe> placement;
    std::optional<std::uint16_t> legacyData; // diagnostic only; no inferred orientation bit masks
    bool operator==(const Cell&) const = default;
};
struct HotbarSlot { std::string identifier; unsigned count{}; };
using Hotbar = std::array<HotbarSlot, 9>;

using Block = Cell;
} // namespace bedrock

namespace bedrock {
enum class BlockError { none, unsupported, wrongThread, staleWorld, budgetExceeded,
    unavailable, invalidRequest, missingItem, stateMismatch, nativeRejected };
struct BlockResult {
    Block block;
    BlockError error = BlockError::none;
    explicit operator bool() const noexcept { return error == BlockError::none && block.kind != CellKind::unknown; }
};
// Native acceptance is not server confirmation. Poll getBlock on later ticks.
struct PlacementResult {
    BlockError error = BlockError::unsupported;
    bool dispatched{}, accepted{};
    explicit operator bool() const noexcept { return error == BlockError::none && dispatched && accepted; }
};
struct BlockPlacement {
    BlockPos target{}, support{};
    Face face = Face::up;
    unsigned hotbarSlot{};
    float yaw{}, pitch{};
    float hitX=.5f, hitY=1.f, hitZ=.5f;
    double maxReach=4.5;
    std::shared_ptr<const BlockProperties> expectedProperties;
    bool allowAugment{};
};
// An adapter must validate thread, world lifetime and capability on every call.
struct BlockSetOptions {
    std::optional<unsigned> hotbarSlot;
    std::optional<float> yaw, pitch;
    double maxReach=4.5;
    std::shared_ptr<const BlockProperties> expectedProperties;
};
class BlockAccess {
public:
    virtual ~BlockAccess() = default;
    virtual BlockResult worldBlock(void*, BlockPos, bool = false) { return {{},BlockError::unsupported}; }
    virtual PlacementResult worldPlaceBlock(void*, const BlockPlacement&) { return {}; }
    virtual PlacementResult worldSetBlock(void*, BlockPos, std::string_view, const BlockSetOptions&) { return {}; }
};
} // namespace bedrock
