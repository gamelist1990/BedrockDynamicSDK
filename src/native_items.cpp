#include <bedrock/native_items.hpp>

#include <nlohmann/json.hpp>

#include <limits>
#include <stdexcept>

namespace bedrock {
namespace {
std::size_t number(const nlohmann::json& parent, const char* key) {
    const auto& value = parent.at(key);
    const auto result = value.is_string()
        ? std::stoull(value.get<std::string>(), nullptr, 0)
        : value.get<std::uint64_t>();
    if (result > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument(std::string{"native_items."} + key + " is too large");
    return static_cast<std::size_t>(result);
}
}

bool NativeItemLayout::valid() const noexcept {
    return actorEntityContextOffset <= 0x10000 && contextEnttRegistryOffset <= 0x10000 &&
           contextEntityIdOffset <= 0x10000 &&
           equipmentComponentHash != 0 && handContainerOffset <= 0x10000 &&
           armorContainerOffset <= 0x10000 && inventoryGetItemVtableSlot <= 0x1000 &&
           localPlayerVtableSlot <= 0x1000 && localPlayerSuppliesOffset <= 0x10000 &&
           suppliesSelectedSlotOffset <= 0x10000 && suppliesInventoryOffset <= 0x10000 &&
           itemReferenceOffset <= 0x10000 && itemCountOffset <= 0x10000 &&
           itemNamespacedIdOffset <= 0x10000 && itemTranslationKeyOffset <= 0x10000 &&
           itemStringSizeOffset <= 0x10000 && itemStringCapacityOffset <= 0x10000 &&
           handSlots[0] <= 255 && handSlots[1] <= 255 &&
           armorSlots[0] <= 255 && armorSlots[1] <= 255 &&
           armorSlots[2] <= 255 && armorSlots[3] <= 255 &&
           localInventorySlots > 0 && localInventorySlots <= 256 &&
           hotbarSlots == 9 && maxStringBytes > 0 && maxStringBytes <= 4096;
}

bool parseNativeItemLayout(std::string_view source, NativeItemLayout& output,
                           std::string& error) {
    try {
        const auto root = nlohmann::json::parse(source.begin(), source.end());
        const auto& data = root.at("native_items");
        if (data.at("schema").get<unsigned>() != 1 ||
            data.at("pointer_size").get<unsigned>() != sizeof(void*))
            throw std::invalid_argument("unsupported native_items schema or pointer size");
        NativeItemLayout value;
        const auto& actor = data.at("actor_context");
        value.actorEntityContextOffset = number(actor, "actor_offset");
        value.contextEnttRegistryOffset = number(actor, "entt_registry_offset");
        value.contextEntityIdOffset = number(actor, "entity_id_offset");
        const auto& equipment = data.at("equipment");
        value.equipmentComponentHash = static_cast<std::uint32_t>(number(equipment, "component_hash"));
        value.handContainerOffset = number(equipment, "hand_container_offset");
        value.armorContainerOffset = number(equipment, "armor_container_offset");
        value.handSlots[0] = number(equipment.at("slots"), "main_hand");
        value.handSlots[1] = number(equipment.at("slots"), "offhand");
        value.armorSlots[0] = number(equipment.at("slots"), "head");
        value.armorSlots[1] = number(equipment.at("slots"), "chest");
        value.armorSlots[2] = number(equipment.at("slots"), "legs");
        value.armorSlots[3] = number(equipment.at("slots"), "feet");
        const auto& inventory = data.at("inventory");
        value.inventoryGetItemVtableSlot = number(inventory, "get_item_vtable_slot");
        value.localInventorySlots = number(inventory, "local_slot_count");
        value.hotbarSlots = number(inventory, "hotbar_slot_count");
        const auto& local = data.at("local_player");
        value.localPlayerVtableSlot = number(local, "get_local_player_vtable_slot");
        value.localPlayerSuppliesOffset = number(local, "supplies_offset");
        value.suppliesSelectedSlotOffset = number(local, "selected_slot_offset");
        value.suppliesInventoryOffset = number(local, "inventory_offset");
        const auto& item = data.at("item_stack");
        value.itemReferenceOffset = number(item, "item_reference_offset");
        value.itemCountOffset = number(item, "count_offset");
        value.itemNamespacedIdOffset = number(item, "namespaced_id_string_offset");
        value.itemTranslationKeyOffset = number(item, "translation_key_string_offset");
        value.itemStringSizeOffset = number(item, "string_size_offset");
        value.itemStringCapacityOffset = number(item, "string_capacity_offset");
        value.maxStringBytes = number(item, "max_string_bytes");
        value.hoverNameAbiVerified = item.value("hover_name_abi_verified", false);
        value.evidence = data.value("evidence", std::string{});
        if (!value.valid()) throw std::invalid_argument("native_items contains out-of-range layout values");
        output = std::move(value);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

} // namespace bedrock
