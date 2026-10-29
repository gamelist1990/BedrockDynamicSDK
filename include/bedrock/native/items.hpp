#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace bedrock {

// Versioned native item layout. Values are accepted only alongside the
// Resolver profile that matched the loaded executable. They are candidates
// until independently tested on that exact build.
struct NativeItemLayout {
    std::size_t actorEntityContextOffset{};
    std::size_t contextEnttRegistryOffset{};
    std::size_t contextEntityIdOffset{};
    std::uint32_t equipmentComponentHash{};
    std::size_t handContainerOffset{};
    std::size_t armorContainerOffset{};
    std::size_t inventoryGetItemVtableSlot{};
    std::size_t localPlayerVtableSlot{};
    std::size_t localPlayerSuppliesOffset{};
    std::size_t suppliesSelectedSlotOffset{};
    std::size_t suppliesInventoryOffset{};
    std::size_t itemReferenceOffset{};
    std::size_t itemCountOffset{};
    std::size_t itemNamespacedIdOffset{};
    std::size_t itemTranslationKeyOffset{};
    std::size_t itemStringSizeOffset{};
    std::size_t itemStringCapacityOffset{};
    std::size_t handSlots[2]{};
    std::size_t armorSlots[4]{};
    std::size_t localInventorySlots{};
    std::size_t hotbarSlots{};
    std::size_t maxStringBytes{};
    bool hoverNameAbiVerified{};
    std::string evidence;

    [[nodiscard]] bool valid() const noexcept;
};

[[nodiscard]] bool parseNativeItemLayout(std::string_view profile,
                                         NativeItemLayout& output,
                                         std::string& error);

} // namespace bedrock
