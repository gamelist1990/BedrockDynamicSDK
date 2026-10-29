#pragma once
#include <bedrock/native_packet_view.hpp>
#include <bedrock/detail/protocol_1_26_60.hpp>
#include <bedrock/game_api.hpp>
#include <cmath>
#include <array>

namespace bedrock {
struct MovementPacketPose {Vec3 position{};std::uint64_t runtimeId{};std::uint8_t onGround{};};
struct MovementPacketEdit {bool applied{},interaction{};};
// Protocol names/IDs/flags come from minecraft-data. Memory offsets below come
// ONLY from the native, exact-build profile; wire ordering cannot supply them.
class NativeMovementPackets {
public:
    void load(const nlohmann::json& root) {
        const auto& motion=root.at("native_packets").at("motion");
        if(motion.value("schema",0)!=1)throw std::runtime_error("unsupported native movement layout");
        const auto field=[](const auto& row,const char* name,std::size_t size) {
            const auto value=native_profile::number(row.at(name));
            if(value<0x30 || value>0x400 || value+size>0x408)throw std::runtime_error("movement field outside bounded native payload");
            return static_cast<std::size_t>(value);
        };
        const auto& a=motion.at("auth_input");const auto& m=motion.at("move_player");
        auth_.position=field(a,"position_offset",12);auth_.vehicle=field(a,"vehicle_offset",8);
        auth_.delta=field(a,"delta_offset",12);auth_.analog=field(a,"analog_move_offset",8);
        auth_.move=field(a,"move_offset",8);auth_.rawMove=field(a,"raw_move_offset",8);
        auth_.flags=field(a,"input_flags_offset",16);
        if(a.at("input_flags_size")!=16 || a.value("input_flags_storage",std::string{})!="native_bitset128")throw std::runtime_error("unsupported native InputData storage");
        move_.position=field(m,"position_offset",12);move_.vehicle=field(m,"vehicle_offset",8);
        move_.runtime=field(m,"runtime_id_offset",8);move_.mode=field(m,"mode_offset",1);move_.ground=field(m,"on_ground_offset",1);
    }
    static bool handles(PacketId id) noexcept{return id==PacketId::MovePlayer||id==PacketId::PlayerAuthInput;}
    bool pose(NativePacketView packet,MovementPacketPose& result) const noexcept {
        if(!handles(packet.id())||!packet.valid())return false;
        result={};
        const bool auth=packet.id()==PacketId::PlayerAuthInput;const auto& layout=auth?auth_:move_;
        std::uint64_t vehicle{};
        if(!read(packet,layout.position,result.position)||!finite(result.position)||!read(packet,layout.vehicle,vehicle)||vehicle!=(auth?UINT64_MAX:0))return false;
        if(!auth){std::uint8_t mode{};
            if(!read(packet,layout.runtime,result.runtimeId)||!read(packet,layout.mode,mode)||mode!=0||
               !read(packet,layout.ground,result.onGround)||result.onGround>1)return false;
        }
        return true;
    }
    MovementPacketEdit anchor(NativePacketView packet,const MovementPacketPose& pose,std::uint64_t runtime) const noexcept {
        MovementPacketPose current;
        if(!finite(pose.position)||!this->pose(packet,current))return {};
        if(packet.id()==PacketId::MovePlayer) {
            if(current.runtimeId!=runtime||pose.onGround>1)return {};
            if(write(packet,move_.position,pose.position)&&write(packet,move_.ground,pose.onGround)&&packet.valid())return {true,false};
            write(packet,move_.position,current.position);write(packet,move_.ground,current.onGround);return {};
        }
        AuthState saved{};
        if(!read(packet,auth_.delta,saved.delta)||!read(packet,auth_.analog,saved.analog)||
           !read(packet,auth_.move,saved.move)||!read(packet,auth_.rawMove,saved.rawMove)||!read(packet,auth_.flags,saved.flags))return {};
        auto flags=saved.flags;using Flag=protocol::v1_26_60::InputData;
        const bool interaction=protocol::v1_26_60::has(flags,Flag::ItemInteract)||protocol::v1_26_60::has(flags,Flag::BlockAction)||
            protocol::v1_26_60::has(flags,Flag::ItemStackRequest)||protocol::v1_26_60::has(flags,Flag::StartUsingItem);
        protocol::v1_26_60::clearMovement(flags);
        const std::array<float,2> zero{};
        if(write(packet,auth_.position,pose.position)&&write(packet,auth_.delta,Vec3{})&&write(packet,auth_.analog,zero)&&
           write(packet,auth_.move,zero)&&write(packet,auth_.rawMove,zero)&&write(packet,auth_.flags,flags)&&packet.valid())return {true,interaction};
        // Restore the original scalar fields after a partial/invalid edit.
        write(packet,auth_.position,current.position);write(packet,auth_.delta,saved.delta);write(packet,auth_.analog,saved.analog);
        write(packet,auth_.move,saved.move);write(packet,auth_.rawMove,saved.rawMove);write(packet,auth_.flags,saved.flags);return {};
    }
private:
    struct Layout {std::size_t position{},vehicle{},delta{},analog{},move{},rawMove{},flags{},runtime{},mode{},ground{};} auth_,move_;
    struct AuthState {Vec3 delta{};std::array<float,2> analog{},move{},rawMove{};std::array<std::uint64_t,2> flags{};};
    static bool finite(Vec3 value) noexcept{return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z)&&std::abs(value.x)<40000000&&std::abs(value.y)<40000000&&std::abs(value.z)<40000000;}
    template<class T> static bool read(NativePacketView packet,std::size_t offset,T& value) noexcept{return native_profile::read(packet.native(),offset,value);}
    template<class T> static bool write(NativePacketView packet,std::size_t offset,const T& value) noexcept {
        __try{std::memcpy(static_cast<std::byte*>(packet.native())+offset,&value,sizeof(value));return true;}
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
};
static_assert(packetIdValue(PacketId::MovePlayer)==protocol::v1_26_60::MovePlayerId);
static_assert(packetIdValue(PacketId::PlayerAuthInput)==protocol::v1_26_60::PlayerAuthInputId);
}
