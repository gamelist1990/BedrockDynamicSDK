#pragma once
#include <bedrock/packet_ids.hpp>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bedrock {
// Bounded wire primitives. No Minecraft object layout, vtable or allocator.
class WireReader {
public:
    explicit WireReader(std::string_view bytes):bytes_(bytes){}
    std::size_t offset() const noexcept{return cursor_;}
    std::size_t remaining() const noexcept{return bytes_.size()-cursor_;}
    std::string_view take(std::size_t count) {
        if(count>remaining())throw std::runtime_error("truncated packet");
        const auto value=bytes_.substr(cursor_,count);cursor_+=count;return value;
    }
    std::uint8_t u8(){return static_cast<std::uint8_t>(take(1)[0]);}
    bool boolean(){const auto value=u8();if(value>1)throw std::runtime_error("invalid wire bool");return value!=0;}
    std::uint64_t varuint(unsigned bits=64) {
        if(bits!=32&&bits!=64)throw std::runtime_error("invalid varint width");
        std::uint64_t value{};
        for(unsigned shift=0;shift<bits;shift+=7){const auto byte=u8();
            const auto left=bits-shift;
            if(left<7&&(byte&0x7f)>=(1u<<left))throw std::runtime_error("wire varint overflow");
            value|=std::uint64_t(byte&0x7f)<<shift;
            if(!(byte&0x80)){if(shift&&byte==0)throw std::runtime_error("noncanonical wire varint");return value;}
        }
        throw std::runtime_error("unterminated wire varint");
    }
    std::int64_t zigzag(unsigned bits=64){const auto n=varuint(bits);return std::bit_cast<std::int64_t>((n>>1)^(std::uint64_t{0}-(n&1)));}
    float f32(){const auto bytes=take(4);std::uint32_t n{};for(unsigned i=0;i<4;++i)n|=std::uint32_t(static_cast<unsigned char>(bytes[i]))<<(i*8);return std::bit_cast<float>(n);}
    std::string_view string(std::size_t maximum=1024*1024){const auto count=varuint(32);if(count>maximum)throw std::runtime_error("wire string limit");return take(static_cast<std::size_t>(count));}
private:
    std::string_view bytes_;std::size_t cursor_{};
};
class WireWriter {
public:
    void bytes(std::string_view bytes){bytes_.append(bytes);}
    void u8(std::uint8_t value){bytes_.push_back(static_cast<char>(value));}
    void boolean(bool value){u8(value?1:0);}
    void varuint(std::uint64_t value){do{const auto byte=static_cast<std::uint8_t>(value&0x7f);value>>=7;u8(byte|(value?0x80:0));}while(value);}
    void zigzag(std::int64_t value){const auto n=std::bit_cast<std::uint64_t>(value);varuint((n<<1)^(std::uint64_t{0}-(n>>63)));}
    void f32(float value){const auto n=std::bit_cast<std::uint32_t>(value);for(unsigned i=0;i<4;++i)u8(static_cast<std::uint8_t>(n>>(i*8)));}
    void string(std::string_view value){if(value.size()>UINT32_MAX)throw std::runtime_error("wire string overflow");varuint(value.size());bytes(value);}
    const std::string& data() const noexcept{return bytes_;}
private:std::string bytes_;
};
class WirePacket {
public:
    static constexpr std::size_t maximumBytes=16*1024*1024;
    explicit WirePacket(std::string bytes):bytes_(std::move(bytes)) {
        if(bytes_.empty()||bytes_.size()>maximumBytes)throw std::runtime_error("wire packet size limit");
        WireReader reader(bytes_);header_=static_cast<std::uint32_t>(reader.varuint(32));
        if(header_>0x3fff)throw std::runtime_error("reserved packet header bits");
        headerSize_=reader.offset();
    }
    unsigned rawId() const noexcept{return header_&0x3ff;}
    PacketId id() const noexcept{return packetIdFromValue(rawId()).value_or(PacketId::Unknown);}
    unsigned senderSubClient() const noexcept{return (header_>>10)&3;}
    unsigned targetSubClient() const noexcept{return (header_>>12)&3;}
    std::string_view payload() const noexcept{return std::string_view(bytes_).substr(headerSize_);}
    const std::string& bytes() const noexcept{return bytes_;}
    bool modified() const noexcept{return modified_;}
    void replacePayload(std::string_view bytes) {
        if(bytes.size()>maximumBytes-headerSize_)throw std::runtime_error("wire packet size limit");
        std::string replacement=bytes_.substr(0,headerSize_);replacement.append(bytes);bytes_.swap(replacement);modified_=true;
    }
private:std::string bytes_;std::uint32_t header_{};std::size_t headerSize_{};bool modified_{};
};
} // namespace bedrock
