#include "item_packet_decode.hpp"

#include <bedrock/packets/wire_packet.hpp>

#include <bit>
#include <cstddef>
#include <limits>

namespace bedrock::sdk::detail {
namespace {
constexpr std::size_t maxItemStates = 8192;
constexpr std::size_t maxNameBytes = 64 * 1024;
constexpr std::size_t maxArrayElements = 4096;

std::int16_t readLi16(WireReader& reader) {
    const auto bytes = reader.take(2);
    const auto value = static_cast<std::uint16_t>(
        static_cast<unsigned char>(bytes[0]) |
        (static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[1])) << 8));
    return std::bit_cast<std::int16_t>(value);
}

std::uint16_t readLu16(WireReader& reader) {
    const auto bytes = reader.take(2);
    return static_cast<std::uint16_t>(
        static_cast<unsigned char>(bytes[0]) |
        (static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[1])) << 8));
}

std::size_t boundedCount(std::int64_t count, std::size_t limit) {
    if (count <= 0 || static_cast<std::uint64_t>(count) > limit) return 0;
    return static_cast<std::size_t>(count);
}

bool skipNbtValue(WireReader& reader, std::uint8_t type, unsigned depth);

// Network little-endian NBT tags carry a type byte, a varint-length name and
// then the value. This mirrors prismarine-nbt's littleVarint schema: strings
// and names use varuint32 lengths; list/array counts use zigzag32.
bool skipNamedNbt(WireReader& reader, unsigned depth) {
    const auto type = reader.u8();
    if (type == 0) return true; // TAG_End
    reader.string(maxNameBytes);
    return skipNbtValue(reader, type, depth);
}

bool skipNbtValue(WireReader& reader, std::uint8_t type, unsigned depth) {
    if (depth > 32) return false;
    switch (type) {
    case 1:  reader.take(1); return true; // Byte
    case 2:  reader.take(2); return true; // Short
    case 3:  reader.zigzag(32); return true; // Int
    case 4:  reader.zigzag(64); return true; // Long
    case 5:  reader.take(4); return true; // Float
    case 6:  reader.take(8); return true; // Double
    case 7:  reader.take(boundedCount(reader.zigzag(32), 4 * 1024 * 1024)); return true; // ByteArray
    case 8:  reader.string(maxNameBytes); return true; // String
    case 9: { // List
        const auto element = reader.u8();
        const auto length = boundedCount(reader.zigzag(32), maxArrayElements);
        for (std::size_t i = 0; i < length; ++i)
            if (!skipNbtValue(reader, element, depth + 1)) return false;
        return true;
    }
    case 10: // Compound: named child tags terminated by TAG_End.
        for (;;) {
            const auto child = reader.u8();
            if (child == 0) return true;
            reader.string(maxNameBytes);
            if (!skipNbtValue(reader, child, depth + 1)) return false;
        }
    case 11: { // IntArray
        const auto length = boundedCount(reader.zigzag(32), maxArrayElements);
        for (std::size_t i = 0; i < length; ++i) reader.zigzag(32);
        return true;
    }
    case 12: { // LongArray
        const auto length = boundedCount(reader.zigzag(32), maxArrayElements);
        for (std::size_t i = 0; i < length; ++i) reader.zigzag(64);
        return true;
    }
    default:
        return false;
    }
}
} // namespace

std::optional<ItemActorPacketPrefix> decodeAddItemActorPrefix(std::string_view payload) {
    try {
        WireReader reader(payload);
        reader.zigzag(64); // entity_id_self
        ItemActorPacketPrefix decoded;
        decoded.runtimeId = reader.varuint(64); // runtime_entity_id
        decoded.networkId = readLi16(reader);   // ItemV4.network_id
        decoded.count = readLu16(reader);       // ItemV4.count
        if (!decoded.runtimeId || !decoded.networkId || !decoded.count) return std::nullopt;
        return decoded;
    } catch (...) {
        return std::nullopt;
    }
}

bool decodeItemRegistry(std::string_view payload,
                        std::unordered_map<std::uint16_t, std::string>& into) {
    try {
        WireReader reader(payload);
        const auto count = reader.varuint(32);
        if (count > maxItemStates) return false;
        std::size_t accepted = 0;
        for (std::uint64_t i = 0; i < count; ++i) {
            std::string name(reader.string(maxNameBytes));
            const auto runtimeId = readLi16(reader);
            reader.boolean();  // component_based
            reader.zigzag(32); // version
            if (!skipNamedNbt(reader, 0)) return false;
            if (!name.empty() && runtimeId) {
                into.insert_or_assign(static_cast<std::uint16_t>(runtimeId), std::move(name));
                ++accepted;
            }
        }
        return accepted != 0;
    } catch (...) {
        return false;
    }
}

} // namespace bedrock::sdk::detail
