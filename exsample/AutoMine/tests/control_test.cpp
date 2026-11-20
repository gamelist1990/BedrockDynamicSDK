#include <bedrock/detail/automation_motion.hpp>
#include <bedrock/detail/automation_input.hpp>
#include "../src/screen_policy.hpp"
#include "../src/locomotion_geometry.hpp"
#include <bedrock/packets/movement_packets.hpp>
#include <cassert>
#include <limits>

using namespace bedrock;
static bool near(float a,float b) { return std::abs(a-b)<.0001f; }
static WirePacket packet(PacketId id,const WireWriter& payload) {
    WireWriter wire;wire.varuint(static_cast<unsigned>(id));wire.bytes(payload.data());
    return WirePacket{wire.data()};
}
int main() {
    // Engine yaw writes must not leak the borrowed simulation direction into
    // the camera, while ordinary analog patches retain engine-owned changes.
    assert(detail::automationRestoredAxis(37.f, -90.f, 37.f, true) == -90.f);
    assert(detail::automationRestoredAxis(45.f, -90.f, 37.f, true) == -90.f);
    assert(detail::automationRestoredAxis(1.f, 0.f, 1.f, false) == 0.f);
    assert(detail::automationRestoredAxis(.5f, 0.f, 1.f, false) == .5f);
    assert(!automine::userScreenActive("hud_screen"));
    for (const auto *screen : {"chat_screen", "inventory_screen", "pause_screen", ""})
        assert(automine::userScreenActive(screen));
    assert(!automine::screenBlocksMovement("chat_screen", true));
    assert(automine::screenBlocksMovement("chat_screen", false));
    assert(automine::screenBlocksMovement("inventory_screen", true));
    assert(automine::screenBlocksMovement("", true));
    // Profile-supplied native masks, not the network InputData enum.
    const std::array<std::uint32_t,15> masks{0x2000,0x4000,0x8000,0x10000,
        0x4000080,0x200001,0x100,0x200,0x400,0x800,0x1000,
        0x2000000,0x1000000,0x800000,0x400000};
    AutomationIntent jumping;
    jumping.jump = true;
    auto flags = detail::automationFlags(jumping,{0,1},0x80000000,0x80000000,
                                          false,false,masks);
    assert((flags.input & masks[4]) == masks[4]);
    assert((flags.held & masks[4]) == masks[4]); // jump physics needs BOTH words
    assert(flags.input & masks[11]);
    assert(!(flags.held & masks[11]));
    assert((flags.input & 0x80000000) && (flags.held & 0x80000000));
    flags = detail::automationFlags(jumping,{0,1},flags.input,flags.held,true,false,masks);
    assert(!(flags.input & masks[11]) && (flags.held & masks[4]) == masks[4]);
    jumping.jump = false;
    flags = detail::automationFlags(jumping,{},flags.input,flags.held,true,false,masks);
    assert(flags.input & masks[12]);
    assert(!(flags.input & masks[4]) && !(flags.held & masks[4]));
    // Position -> sprint runup -> takeoff -> steering -> supported landing.
    // Both native words must keep sprint throughout; jump edges are input-only.
    bool lastJump = false;
    unsigned flight = 0;
    for (const int stage : {0, 1, 1, 2, 2, 2, 2, 3}) {
        const auto drive = automine::parkourDrive(stage, stage == 2 ? flight++ : 0);
        AutomationIntent sprintJump;
        sprintJump.forward = drive.forward;
        sprintJump.sprint = drive.sprint;
        sprintJump.jump = drive.jump;
        sprintJump.sprintJump = stage == 1 || stage == 2;
        sprintJump.yaw = 37;
        for (float camera : {-180.f, -90.f, 0.f, 90.f, 180.f}) {
            const auto simYaw = automationSimulationYaw(sprintJump, camera);
            const auto simMove = automationMove(sprintJump, simYaw);
            assert(near(simMove.forward, drive.forward ? 1.f : 0.f));
            assert(near(simMove.left, 0.f));
            const auto words = detail::automationFlags(sprintJump, simMove,
                0x80000000, 0x80000000, lastJump, false, masks);
            assert(bool(words.input & masks[6]) == drive.sprint);
            assert(bool(words.held & masks[6]) == drive.sprint);
            assert(bool(words.input & masks[0]) == drive.forward);
            assert(bool(words.held & masks[0]) == drive.forward);
            assert(bool(words.input & masks[4]) == drive.jump);
            assert(bool(words.held & masks[4]) == drive.jump);
            assert(!(words.held & (masks[11] | masks[12])));
            assert(bool(words.input & masks[11]) == (drive.jump && !lastJump));
            assert(bool(words.input & masks[12]) == (!drive.jump && lastJump));
        }
        lastJump = drive.jump;
    }
    AutomationIntent intent;intent.forward=1;intent.yaw=0;
    auto move=automationMove(intent,0);assert(near(move.forward,1)&&near(move.left,0));
    move=automationMove(intent,90);assert(near(move.forward,0)&&near(move.left,1));
    move=automationMove(intent,180);assert(near(move.forward,-1)&&near(move.left,0));
    intent.left=1;move=automationMove(intent,0);assert(near(std::hypot(move.forward,move.left),1));
    intent.yaw=std::numeric_limits<float>::quiet_NaN();
    move=automationMove(intent,0);assert(move.forward==0&&move.left==0);

    WireWriter payload;
    for(float f:{1.f,2.f,3.f,4.f,5.f,0.f,1.f,2.f})payload.f32(f);
    payload.varuint(4);
    for(auto flag:{protocol::InputData::Up,protocol::InputData::Jumping,
        protocol::InputData::BlockAction,protocol::InputData::ItemStackRequest})
        payload.zigzag(static_cast<unsigned>(flag));
    payload.varuint(1);payload.varuint(0);payload.zigzag(0);
    payload.f32(0);payload.f32(1);payload.varuint(123);
    payload.f32(.1f);payload.f32(.2f);payload.f32(.3f);
    const auto optionsAt=payload.data().size();
    // Conditional transaction/action bytes are opaque to the rotation edit.
    const std::string options="\1block-action-payload\1stack-request\0";
    payload.bytes(options);
    for(float f:{0.f,1.f,1.f,0.f,0.f,0.f,1.f})payload.f32(f);
    auto auth=packet(PacketId::PlayerAuthInput,payload);
    const auto original=auth.bytes();
    MovementPackets codec;
    assert(codec.automate(auth,90,-25,1,0));
    MovementPacketPose pose;assert(codec.pose(auth,pose));
    assert(pose.position.x==3&&pose.position.y==4&&pose.position.z==5);
    WireReader reader(auth.payload());
    assert(reader.f32()==-25&&reader.f32()==90);
    reader.take(12);assert(reader.f32()==1&&reader.f32()==0);assert(reader.f32()==90);
    const auto count=reader.varuint();bool jump=false,block=false,stack=false,left=false,up=false;
    for(std::uint64_t i=0;i<count;++i) {
        const auto f=reader.zigzag();jump|=f==static_cast<int>(protocol::InputData::Jumping);
        block|=f==static_cast<int>(protocol::InputData::BlockAction);
        stack|=f==static_cast<int>(protocol::InputData::ItemStackRequest);
        left|=f==static_cast<int>(protocol::InputData::Left);up|=f==static_cast<int>(protocol::InputData::Up);
    }
    assert(jump&&block&&stack&&left&&!up);
    reader.varuint();reader.varuint();reader.zigzag();reader.take(8);
    assert(reader.varuint()==123);reader.take(12);
    assert(reader.take(options.size())==options);
    assert(payload.data().substr(optionsAt,options.size())==options);
    assert(reader.f32()==1&&reader.f32()==0);reader.take(12);
    assert(reader.f32()==1&&reader.f32()==0);
    const auto edited=auth.bytes();
    assert(!codec.automate(auth,0,100,0,1)&&auth.bytes()==edited);
    assert(!codec.look(auth,std::numeric_limits<float>::infinity(),0)&&auth.bytes()==edited);
    assert(original!=edited);

    WireWriter player;player.varuint(42);
    for(float f:{3.f,4.f,5.f,1.f,2.f,2.f})player.f32(f);
    player.u8(0);player.boolean(true);player.varuint(0);player.boolean(false);player.varuint(321);
    auto legacy=packet(PacketId::MovePlayer,player);
    const auto before=std::string(legacy.payload());
    assert(codec.look(legacy,450,-10));
    assert(legacy.payload().substr(0,13)==std::string_view(before).substr(0,13));
    assert(legacy.payload().substr(25)==std::string_view(before).substr(25));
    WireReader look(legacy.payload());look.varuint();look.take(12);
    assert(look.f32()==-10&&look.f32()==90&&look.f32()==90);
    assert(codec.pose(legacy,pose)&&pose.runtimeId==42&&pose.onGround==1);
}
