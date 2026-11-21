#pragma once
#include <cstdint>
#include <optional>
#include <string_view>

namespace bedrock {

// Protocol IDs only. These values describe neither a native Packet object
// layout nor a capability to send a packet. Unknown and private IDs stay raw.
enum class PacketId : std::uint16_t {
#define BEDROCK_PACKET_ID(name, value) name = value,
#include <bedrock/generated/packet_ids.inc>
#undef BEDROCK_PACKET_ID
    MobEquipment = PlayerEquipment,
    LevelChunk = FullChunkData,
    ModalFormRequest = ShowModalForm,
    SetLocalPlayerAsInitialized = SetLocalPlayerAsInit,
    PlayerAuthInput = PlayerAuthInputPacket,
    Unknown = 0xFFFF
};
using PacketList = PacketId;
using MinecraftPacketIds = PacketId;

[[nodiscard]] constexpr std::uint16_t packetIdValue(PacketId id) noexcept {
    return static_cast<std::uint16_t>(id);
}
[[nodiscard]] constexpr std::string_view packetName(PacketId id) noexcept {
    switch (id) {
#define BEDROCK_PACKET_ID(name, value)                                                             \
    case PacketId::name:                                                                           \
        return #name;
#include <bedrock/generated/packet_ids.inc>
#undef BEDROCK_PACKET_ID
    default:
        return "Unknown";
    }
}
[[nodiscard]] constexpr std::optional<PacketId> packetIdFromValue(std::uint32_t value) noexcept {
    // Check before narrowing: high bits must not alias a known ID.
    if (value > 0x3FF)
        return std::nullopt;
    const auto id = static_cast<PacketId>(value);
    return packetName(id) != "Unknown" ? std::optional{id} : std::nullopt;
}
[[nodiscard]] constexpr std::optional<PacketId> packetIdFromName(std::string_view name) noexcept {
#define BEDROCK_PACKET_ID(symbol, value)                                                           \
    if (name == #symbol)                                                                           \
        return PacketId::symbol;
#include <bedrock/generated/packet_ids.inc>
#undef BEDROCK_PACKET_ID
    if (name == "MobEquipment")
        return PacketId::MobEquipment;
    if (name == "LevelChunk")
        return PacketId::LevelChunk;
    if (name == "ModalFormRequest")
        return PacketId::ModalFormRequest;
    if (name == "SetLocalPlayerAsInitialized")
        return PacketId::SetLocalPlayerAsInitialized;
    if (name == "PlayerAuthInput")
        return PacketId::PlayerAuthInput;
    return std::nullopt;
}
// Use only after decoding the wire header's unsigned VarInt. This explicitly
// strips the sender/target subclient bits; packetIdFromValue does not strip them.
[[nodiscard]] constexpr std::optional<PacketId> packetIdFromHeader(std::uint32_t header) noexcept {
    return packetIdFromValue(header & 0x3FFu);
}

} // namespace bedrock
