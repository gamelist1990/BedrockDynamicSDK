#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace bedrock::sdk {

enum class CellKind { unknown, air, block };
enum class Face { down, up, north, south, west, east };
enum class Hand { main };

struct BlockPos {
    int x{}, y{}, z{};
    auto operator<=>(const BlockPos&) const = default;
};
using StateValue = std::variant<int, std::string>;
struct BlockProperties { std::map<std::string, StateValue> values; };
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
    std::shared_ptr<const BlockProperties> properties;
    bool supportsPlacement = false;
    bool nativePlacementCandidate = false;
    std::optional<PlacementRecipe> placement;
    std::optional<std::uint16_t> legacyData;
    bool operator==(const Cell&) const = default;
};
struct HotbarSlot { std::string identifier; unsigned count{}; };
using Hotbar = std::array<HotbarSlot, 9>;

} // namespace bedrock::sdk

namespace bedrock::sdk::vanilla {
struct MinecraftBlockType { constexpr explicit MinecraftBlockType(std::string_view value) noexcept:value_(value){}; constexpr std::string_view value() const noexcept{return value_;} private: std::string_view value_;};
inline constexpr MinecraftBlockType Stone{"minecraft:stone"};
inline constexpr MinecraftBlockType Dirt{"minecraft:dirt"};
inline constexpr MinecraftBlockType Glass{"minecraft:glass"};
inline constexpr MinecraftBlockType OakPlanks{"minecraft:oak_planks"};
inline constexpr MinecraftBlockType OakLog{"minecraft:oak_log"};
inline constexpr MinecraftBlockType Air{"minecraft:air"};
} // namespace bedrock::sdk::vanilla
