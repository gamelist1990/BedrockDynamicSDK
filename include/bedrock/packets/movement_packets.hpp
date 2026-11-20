#pragma once
#include <bedrock/packets/wire_packet.hpp>
#include <bedrock/generated/protocol.hpp>
#include <bedrock/detail/hook_dispatch.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace bedrock {
struct MovementPacketPose {Vec3 position{};std::uint64_t runtimeId{};std::uint8_t onGround{};};
struct MovementPacketEdit {bool applied{},interaction{};};
// Wire codecs for the imported 1.26.51 / 1.26.60 movement schema. The game
// constructs/validates the native packet; this class only edits its wire copy.
class MovementPackets {
public:
    static bool handles(PacketId id) noexcept{return id==PacketId::MovePlayer||id==PacketId::PlayerAuthInput;}
    // Change only the server-facing look. Local view, movement prediction,
    // transactions and pending block actions keep their original bytes.
    bool look(WirePacket& packet, float yaw, float pitch) const noexcept {
        try {
            if (!std::isfinite(yaw) || !std::isfinite(pitch) || pitch < -90.f || pitch > 90.f)
                return false;
            const auto fields = parse(packet);
            std::string edited(packet.payload());
            const auto put = [&](std::size_t at, float value) {
                WireWriter writer; writer.f32(value); edited.replace(at, 4, writer.data());
            };
            yaw = std::remainder(yaw, 360.f);
            put(fields.pitch, pitch); put(fields.yaw, yaw); put(fields.headYaw, yaw);
            packet.replacePayload(edited); return true;
        } catch (...) { return false; }
    }
    bool automate(WirePacket& packet, float yaw, float pitch, float left, float forward) const noexcept {
        try {
            if (!std::isfinite(yaw) || !std::isfinite(pitch) || pitch < -90.f || pitch > 90.f ||
                !std::isfinite(left) || !std::isfinite(forward) || std::hypot(left, forward) > 1.001f)
                return false;
            const auto fields = parse(packet);
            std::string edited(packet.payload());
            const auto put = [&](std::size_t at, float value) {
                WireWriter writer; writer.f32(value); edited.replace(at, 4, writer.data());
            };
            yaw = std::remainder(yaw, 360.f);
            put(fields.pitch, pitch); put(fields.yaw, yaw); put(fields.headYaw, yaw);
            if (packet.id() == PacketId::PlayerAuthInput) {
                for (auto at : {fields.move, fields.analog, fields.raw}) {
                    put(at, left); put(at + 4, forward);
                }
                using I = protocol::InputData;
                constexpr std::array directions{I::Up,I::Down,I::Left,I::Right,I::UpLeft,I::UpRight};
                std::vector<unsigned> flags;
                for (auto flag : fields.flags)
                    if (std::none_of(directions.begin(),directions.end(),
                        [flag](auto direction){return flag==static_cast<unsigned>(direction);})) flags.push_back(flag);
                const bool values[]{forward>.01f,forward<-.01f,left>.01f,left<-.01f,
                    forward>.01f&&left>.01f,forward>.01f&&left<-.01f};
                for (std::size_t i=0;i<directions.size();++i)
                    if(values[i])flags.push_back(static_cast<unsigned>(directions[i]));
                std::sort(flags.begin(),flags.end());
                WireWriter replacement;replacement.varuint(flags.size());
                for(auto flag:flags)replacement.zigzag(flag);
                edited.replace(fields.flagsBegin,fields.flagsEnd-fields.flagsBegin,replacement.data());
            }
            packet.replacePayload(edited);return true;
        } catch (...) { return false; }
    }
    bool pose(const WirePacket& packet,MovementPacketPose& result) const noexcept {
        try{const auto fields=parse(packet);result=fields.pose;return true;}catch(...){return false;}
    }
    MovementPacketEdit anchor(WirePacket& packet,const MovementPacketPose& pose,std::uint64_t runtime) const noexcept {
        try {
            if(!finite(pose.position)||pose.onGround>1)return {};
            const auto fields=parse(packet);std::string edited(packet.payload());
            if(packet.id()==PacketId::MovePlayer){
                if(fields.pose.runtimeId!=runtime)return {};
                edited[fields.ground]=static_cast<char>(pose.onGround);putVector(edited,fields.position,pose.position);
                packet.replacePayload(edited);return {true,false};
            }
            putVector(edited,fields.position,pose.position);putVector(edited,fields.delta,{});
            putZero2(edited,fields.move);putZero2(edited,fields.analog);putZero2(edited,fields.raw);
            std::vector<unsigned> kept;
            for(auto flag:fields.flags){
                const bool movement=std::any_of(protocol::movementControls.begin(),protocol::movementControls.end(),
                    [flag](auto value){return static_cast<unsigned>(value)==flag;});
                if(!movement)kept.push_back(flag);
            }
            WireWriter flags;flags.varuint(kept.size());for(auto flag:kept)flags.zigzag(flag);
            // Splice only the enum array. All conditional transaction, stack,
            // block action and acknowledgement payload bytes survive unchanged.
            edited.replace(fields.flagsBegin,fields.flagsEnd-fields.flagsBegin,flags.data());
            packet.replacePayload(edited);return {true,fields.interaction};
        }catch(...){return {};}
    }
private:
    struct Fields {
        MovementPacketPose pose{};std::size_t position{},ground{},move{},delta{},analog{},raw{},flagsBegin{},flagsEnd{},pitch{},yaw{},headYaw{};
        std::vector<unsigned> flags;bool interaction{};
    };
    static bool finite(Vec3 value) noexcept{return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z)&&std::abs(value.x)<40000000&&std::abs(value.y)<40000000&&std::abs(value.z)<40000000;}
    static float scalar(WireReader& reader){const auto value=reader.f32();if(!std::isfinite(value))throw std::runtime_error("nonfinite movement field");return value;}
    static Vec3 vector(WireReader& reader){return {scalar(reader),scalar(reader),scalar(reader)};}
    static void vector2(WireReader& reader){scalar(reader);scalar(reader);}
    static Fields parse(const WirePacket& packet) {
        if(!handles(packet.id()))throw std::runtime_error("unsupported movement packet ID");
        Fields fields;WireReader reader(packet.payload());
        if(packet.id()==PacketId::MovePlayer){
            fields.pose.runtimeId=reader.varuint(32);fields.position=reader.offset();fields.pose.position=vector(reader);
            fields.pitch=reader.offset();scalar(reader);fields.yaw=reader.offset();scalar(reader);
            fields.headYaw=reader.offset();scalar(reader);
            if(reader.u8()!=0)throw std::runtime_error("movement reset/teleport/rotation passthrough");
            fields.ground=reader.offset();fields.pose.onGround=reader.boolean();
            if(reader.varuint(32)!=0||reader.boolean())throw std::runtime_error("vehicle or teleport movement");
            reader.varuint();if(reader.remaining())throw std::runtime_error("movement schema mismatch");
        }else{
            fields.pitch=reader.offset();scalar(reader);fields.yaw=reader.offset();scalar(reader);
            fields.position=reader.offset();fields.pose.position=vector(reader);
            fields.move=reader.offset();vector2(reader);fields.headYaw=reader.offset();scalar(reader);fields.flagsBegin=reader.offset();
            const auto count=reader.varuint(32);if(count>128)throw std::runtime_error("InputData array limit");
            std::array<bool,128> seen{};
            for(std::size_t i=0;i<count;++i){const auto flag=reader.zigzag(32);
                if(flag<0||flag>65||seen[static_cast<unsigned>(flag)])throw std::runtime_error("unsupported or duplicate InputData");
                const auto index=static_cast<unsigned>(flag);seen[index]=true;fields.flags.push_back(index);
                if(index==34||index==35||index==36||index==53)fields.interaction=true;
            }
            if(seen[45])throw std::runtime_error("predicted vehicle movement");
            fields.flagsEnd=reader.offset();reader.varuint(32);reader.varuint(32);reader.zigzag(32);
            vector2(reader);reader.varuint();fields.delta=reader.offset();vector(reader);
            // Five option fields precede the fixed trailer. Preserve their
            // complete encoding, including nested transactions and unknown data.
            if(reader.remaining()<5+28)throw std::runtime_error("truncated movement options/trailer");
            fields.analog=packet.payload().size()-28;fields.raw=packet.payload().size()-8;
            WireReader tail(packet.payload().substr(fields.analog));vector2(tail);vector(tail);vector2(tail);
        }
        if(!finite(fields.pose.position))throw std::runtime_error("movement position out of bounds");
        return fields;
    }
    static void putVector(std::string& bytes,std::size_t offset,Vec3 value) {
        WireWriter writer;writer.f32(value.x);writer.f32(value.y);writer.f32(value.z);bytes.replace(offset,12,writer.data());
    }
    static void putZero2(std::string& bytes,std::size_t offset){WireWriter writer;writer.f32(0);writer.f32(0);bytes.replace(offset,8,writer.data());}
};
} // namespace bedrock
