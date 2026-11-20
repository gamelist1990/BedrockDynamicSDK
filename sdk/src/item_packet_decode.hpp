#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace bedrock::sdk::detail {

// The leading fields needed from `packet_add_item_entity` (AddItemActor):
// entity_id_self, runtime_entity_id, then ItemV4.network_id / ItemV4.count.
struct ItemActorPacketPrefix {
    std::uint64_t runtimeId{};
    std::int16_t networkId{};
    std::uint16_t count{};
};

// `ProtocolCodec` cannot decode this packet: ItemV4.extra is an `encapsulated`
// native type. Read only the bounded prefix and leave the trailing NBT alone.
std::optional<ItemActorPacketPrefix> decodeAddItemActorPrefix(std::string_view payload);

// `packet_item_registry` (ItemRegistryPacket) is a single `itemstates` array of
// {name, runtime_id, component_based, version, nbt}. Its `nbt` field is also a
// native type, so decode the four leading fields and bounds-skip the NBT value.
bool decodeItemRegistry(std::string_view payload,
                        std::unordered_map<std::uint16_t, std::string>& into);

} // namespace bedrock::sdk::detail
