#pragma once
#include <bedrock/wire_packet.hpp>
#include <bedrock/detail/protocol_1_26_60.hpp>
#include <bedrock/game_api.hpp>
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
                const bool movement=std::any_of(protocol::v1_26_60::movementControls.begin(),protocol::v1_26_60::movementControls.end(),
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
        MovementPacketPose pose{};std::size_t position{},ground{},move{},delta{},analog{},raw{},flagsBegin{},flagsEnd{};
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
            scalar(reader);scalar(reader);scalar(reader);
            if(reader.u8()!=0)throw std::runtime_error("movement reset/teleport/rotation passthrough");
            fields.ground=reader.offset();fields.pose.onGround=reader.boolean();
            if(reader.varuint(32)!=0||reader.boolean())throw std::runtime_error("vehicle or teleport movement");
            reader.varuint();if(reader.remaining())throw std::runtime_error("movement schema mismatch");
        }else{
            scalar(reader);scalar(reader);fields.position=reader.offset();fields.pose.position=vector(reader);
            fields.move=reader.offset();vector2(reader);scalar(reader);fields.flagsBegin=reader.offset();
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
        if(!finite(fields.pose.position))throw std::runtime_error("movement position out of bounds");return fields;
    }
    static void putVector(std::string& bytes,std::size_t offset,Vec3 value) {
        WireWriter writer;writer.f32(value.x);writer.f32(value.y);writer.f32(value.z);bytes.replace(offset,12,writer.data());
    }
    static void putZero2(std::string& bytes,std::size_t offset){WireWriter writer;writer.f32(0);writer.f32(0);bytes.replace(offset,8,writer.data());}
};
} // namespace bedrock
