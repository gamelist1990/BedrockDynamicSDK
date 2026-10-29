#pragma once
#include <bedrock/packets/wire_packet.hpp>
#include <bedrock/generated/protocol.hpp>
#include <nlohmann/json.hpp>
#include <vector>

namespace bedrock {
// Bounded schema interpreter over the complete, unmodified protocol.json.
// Supports the wire primitives/containers used by PlayerList and many ordinary
// packets. Special native types (NBT, bitflags, etc.) fail explicitly; callers
// can always use WireReader/Writer for an unsupported packet's raw payload.
class ProtocolCodec {
  public:
    using Json = nlohmann::json;
    Json decode(PacketId id, std::string_view payload) const {
        if (payload.size() > WirePacket::maximumBytes)
            throw std::runtime_error("protocol payload limit");
        WireReader reader(payload);
        Budget budget;
        Context context;
        auto value = read(packetType(id), reader, context, budget, 0);
        if (reader.remaining())
            throw std::runtime_error("protocol trailing payload bytes");
        return value;
    }
    std::string encode(PacketId id, const Json &value) const {
        WireWriter writer;
        Budget budget;
        Context context;
        write(packetType(id), value, writer, context, budget, 0);
        if (writer.data().size() > WirePacket::maximumBytes)
            throw std::runtime_error("protocol output limit");
        return writer.data();
    }
    static const Json &types() {
        static const Json source = [] {
            std::string text;
            for (const auto chunk : protocol::sourceChunks)
                text.append(chunk);
            return Json::parse(text);
        }();
        return source.at("types");
    }

  private:
    using Context = std::vector<const Json *>;
    struct Budget {
        unsigned nodes{};
        void step(unsigned depth) {
            if (depth > 64 || ++nodes > 65536)
                throw std::runtime_error("protocol complexity limit");
        }
    };
    static Json packetType(PacketId id) {
        return "packet_" + std::string(protocol::packetName(static_cast<std::uint16_t>(id)));
    }
    static const Json &lookup(std::string path, const Context &context) {
        if (context.empty())
            throw std::runtime_error("protocol context unavailable");
        auto index = context.size() - 1;
        while (path.starts_with("../")) {
            if (!index)
                throw std::runtime_error("protocol parent context missing");
            --index;
            path.erase(0, 3);
        }
        if (path.starts_with('/')) {
            index = 0;
            path.erase(0, 1);
        }
        const Json *node = context[index];
        std::size_t start{};
        for (;;) {
            const auto end = path.find('/', start);
            node = &node->at(path.substr(start, end - start));
            if (end == std::string::npos)
                return *node;
            start = end + 1;
        }
    }
    static std::string key(const Json &value) {
        return value.is_string() ? value.get<std::string>() : value.dump();
    }
    static const Json &branch(const Json &spec, const Context &context) {
        const auto value = key(lookup(spec.at("compareTo").get<std::string>(), context));
        if (spec.at("fields").contains(value))
            return spec.at("fields").at(value);
        if (spec.contains("default"))
            return spec.at("default");
        throw std::runtime_error("unsupported protocol switch value: " + value);
    }
    static std::size_t count(const Json &spec, WireReader *reader, const Context &context,
                             Budget &budget, unsigned depth) {
        Json n;
        if (spec.contains("countType")) {
            if (!reader)
                throw std::runtime_error("protocol count reader missing");
            n = read(spec.at("countType"), *reader, context, budget, depth + 1);
        } else if (spec.at("count").is_string())
            n = lookup(spec.at("count").get<std::string>(), context);
        else
            n = spec.at("count");
        if (!n.is_number_integer() || n.get<std::int64_t>() < 0)
            throw std::runtime_error("invalid protocol count");
        return n.get<std::size_t>();
    }
    static std::uint64_t fixed(WireReader &r, unsigned bytes, bool little) {
        const auto data = r.take(bytes);
        std::uint64_t value{};
        for (unsigned i = 0; i < bytes; ++i)
            value |= std::uint64_t(static_cast<unsigned char>(data[i]))
                     << (8 * (little ? i : bytes - 1 - i));
        return value;
    }
    static bool integer(std::string_view name, unsigned &bytes, bool &little, bool &sign) {
        little = name.starts_with('l');
        if (little)
            name.remove_prefix(1);
        if (name.size() < 2 || (name[0] != 'i' && name[0] != 'u'))
            return false;
        sign = name[0] == 'i';
        const auto bits = name.substr(1);
        bytes = bits == "8" ? 1 : bits == "16" ? 2 : bits == "32" ? 4 : bits == "64" ? 8 : 0;
        return bytes != 0;
    }
    static Json read(const Json &type, WireReader &r, Context context, Budget &budget,
                     unsigned depth) {
        budget.step(depth);
        if (type.is_string()) {
            const auto name = type.get<std::string>();
            unsigned bytes{};
            bool little{}, sign{};
            if (name == "void")
                return nullptr;
            if (name == "bool")
                return r.boolean();
            if (name == "varint")
                return r.varuint(32);
            if (name == "varint64")
                return r.varuint();
            if (name == "zigzag32")
                return r.zigzag(32);
            if (name == "zigzag64")
                return r.zigzag();
            if (name == "uuid") {
                const auto value = r.take(16);
                return Json::binary(std::vector<std::uint8_t>(value.begin(), value.end()));
            }
            if (name == "lf32")
                return r.f32();
            if (integer(name, bytes, little, sign)) {
                const auto value = fixed(r, bytes, little);
                if (!sign)
                    return Json(value);
                const auto shift = 64 - bytes * 8;
                return Json(std::bit_cast<std::int64_t>(value << shift) >> shift);
            }
            if (types().contains(name) && types().at(name) != "native")
                return read(types().at(name), r, context, budget, depth + 1);
            throw std::runtime_error("unsupported protocol type: " + name);
        }
        const auto kind = type.at(0).get<std::string>();
        const auto &spec = type.at(1);
        if (kind == "container") {
            Json result = Json::object();
            context.push_back(&result);
            for (const auto &field : spec) {
                auto value = read(field.at("type"), r, context, budget, depth + 1);
                if (field.value("anon", false)) {
                    if (value.is_object())
                        result.update(value);
                    else if (!value.is_null())
                        throw std::runtime_error("nonobject anonymous protocol field");
                } else
                    result[field.at("name").get<std::string>()] = std::move(value);
            }
            return result;
        }
        if (kind == "mapper") {
            auto value = read(spec.at("type"), r, context, budget, depth + 1);
            const auto k = key(value);
            return spec.at("mappings").contains(k) ? spec.at("mappings").at(k) : value;
        }
        if (kind == "switch")
            return read(branch(spec, context), r, context, budget, depth + 1);
        if (kind == "optionalOnRemaining")
            return r.remaining() ? read(spec.at("type"), r, context, budget, depth + 1) : Json();
        if (kind == "pstring" || kind == "buffer") {
            const auto size = count(spec, &r, context, budget, depth);
            if (size > 4 * 1024 * 1024)
                throw std::runtime_error("protocol string/buffer limit");
            const auto value = r.take(size);
            return kind == "pstring"
                       ? Json(std::string(value))
                       : Json::binary(std::vector<std::uint8_t>(value.begin(), value.end()));
        }
        if (kind == "array" || kind == "maybeIncompleteArray") {
            const auto size = count(spec, &r, context, budget, depth);
            if (size > 4096)
                throw std::runtime_error("protocol array limit");
            Json result = Json::array();
            for (std::size_t i = 0; i < size; ++i) {
                if (kind == "maybeIncompleteArray" && !r.remaining())
                    break;
                result.push_back(read(spec.at("type"), r, context, budget, depth + 1));
            }
            return result;
        }
        throw std::runtime_error("unsupported protocol constructor: " + kind);
    }
    static void writeFixed(WireWriter &w, std::uint64_t value, unsigned bytes, bool little) {
        for (unsigned i = 0; i < bytes; ++i)
            w.u8(static_cast<std::uint8_t>(value >> (8 * (little ? i : bytes - 1 - i))));
    }
    static void writeCount(const Json &spec, std::size_t size, WireWriter &w, Context context,
                           Budget &budget, unsigned depth) {
        if (spec.contains("countType"))
            write(spec.at("countType"), Json(size), w, context, budget, depth + 1);
        else if (count(spec, nullptr, context, budget, depth) != size)
            throw std::runtime_error("protocol fixed count differs");
    }
    static void write(const Json &type, const Json &value, WireWriter &w, Context context,
                      Budget &budget, unsigned depth) {
        budget.step(depth);
        if (w.data().size() > WirePacket::maximumBytes)
            throw std::runtime_error("protocol output limit");
        if (type.is_string()) {
            const auto name = type.get<std::string>();
            unsigned bytes{};
            bool little{}, sign{};
            if (name == "void")
                return;
            if (name == "bool") {
                w.boolean(value.get<bool>());
                return;
            }
            if (name == "varint" || name == "varint64") {
                const auto n = value.get<std::uint64_t>();
                if ((value.is_number_integer() && !value.is_number_unsigned() &&
                     value.get<std::int64_t>() < 0) ||
                    (name == "varint" && n > UINT32_MAX))
                    throw std::runtime_error("wire varint value overflow");
                w.varuint(n);
                return;
            }
            if (name == "zigzag32" || name == "zigzag64") {
                const auto n = value.get<std::int64_t>();
                if (name == "zigzag32" && (n < INT32_MIN || n > INT32_MAX))
                    throw std::runtime_error("wire zigzag value overflow");
                w.zigzag(n);
                return;
            }
            if (name == "uuid") {
                const auto &data = value.get_binary();
                if (data.size() != 16)
                    throw std::runtime_error("UUID requires 16 raw bytes");
                w.bytes({reinterpret_cast<const char *>(data.data()), data.size()});
                return;
            }
            if (name == "lf32") {
                w.f32(value.get<float>());
                return;
            }
            if (integer(name, bytes, little, sign)) {
                std::uint64_t n{};
                if (sign) {
                    const auto v = value.get<std::int64_t>();
                    if (bytes < 8 && (v < -(std::int64_t{1} << (bytes * 8 - 1)) ||
                                      v > ((std::int64_t{1} << (bytes * 8 - 1)) - 1)))
                        throw std::runtime_error("wire signed integer overflow");
                    n = std::bit_cast<std::uint64_t>(v);
                } else {
                    if (value.is_number_integer() && !value.is_number_unsigned() &&
                        value.get<std::int64_t>() < 0)
                        throw std::runtime_error("negative wire unsigned integer");
                    n = value.get<std::uint64_t>();
                    if (bytes < 8 && n >= (std::uint64_t{1} << (bytes * 8)))
                        throw std::runtime_error("wire unsigned integer overflow");
                }
                writeFixed(w, n, bytes, little);
                return;
            }
            if (types().contains(name) && types().at(name) != "native") {
                write(types().at(name), value, w, context, budget, depth + 1);
                return;
            }
            throw std::runtime_error("unsupported protocol type: " + name);
        }
        const auto kind = type.at(0).get<std::string>();
        const auto &spec = type.at(1);
        if (kind == "container") {
            context.push_back(&value);
            for (const auto &field : spec) {
                if (field.value("anon", false))
                    write(field.at("type"), value, w, context, budget, depth + 1);
                else
                    write(field.at("type"), value.at(field.at("name").get<std::string>()), w,
                          context, budget, depth + 1);
            }
            return;
        }
        if (kind == "mapper") {
            Json mapped = value;
            if (value.is_string()) {
                bool found = false;
                for (auto it = spec.at("mappings").begin(); it != spec.at("mappings").end(); ++it)
                    if (it.value() == value) {
                        mapped = std::stoll(it.key());
                        found = true;
                        break;
                    }
                if (!found)
                    throw std::runtime_error("unknown protocol enum name");
            }
            write(spec.at("type"), mapped, w, context, budget, depth + 1);
            return;
        }
        if (kind == "switch") {
            write(branch(spec, context), value, w, context, budget, depth + 1);
            return;
        }
        if (kind == "optionalOnRemaining") {
            if (!value.is_null())
                write(spec.at("type"), value, w, context, budget, depth + 1);
            return;
        }
        if (kind == "pstring" || kind == "buffer") {
            std::string_view bytes;
            if (kind == "pstring")
                bytes = value.get_ref<const std::string &>();
            else {
                const auto &data = value.get_binary();
                bytes = {reinterpret_cast<const char *>(data.data()), data.size()};
            }
            if (bytes.size() > 4 * 1024 * 1024 ||
                bytes.size() + w.data().size() > WirePacket::maximumBytes)
                throw std::runtime_error("protocol string/output limit");
            writeCount(spec, bytes.size(), w, context, budget, depth);
            w.bytes(bytes);
            return;
        }
        if (kind == "array" || kind == "maybeIncompleteArray") {
            if (!value.is_array() || value.size() > 4096)
                throw std::runtime_error("protocol array limit/type");
            writeCount(spec, value.size(), w, context, budget, depth);
            for (const auto &item : value)
                write(spec.at("type"), item, w, context, budget, depth + 1);
            return;
        }
        throw std::runtime_error("unsupported protocol constructor: " + kind);
    }
};
} // namespace bedrock
