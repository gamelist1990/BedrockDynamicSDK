#pragma once
#include <bedrock/wire_packet.hpp>
#include <bedrock/hook_runtime.h>
#include <bedrock/detail/native_profile_functions.hpp>
#include <array>
#include <optional>

namespace bedrock {
class PacketWireSendEvent {
public:
    PacketWireSendEvent(void* sender,WirePacket& packet):sender_(sender),packet_(packet){}
    void* sender() const noexcept{return sender_;}
    PacketId id() const noexcept{return packet_.id();}
    WirePacket& packet() noexcept{return packet_;}
    void cancel() noexcept{cancelled_=true;}
    bool cancelled() const noexcept{return cancelled_;}
private:void* sender_{};WirePacket& packet_;bool cancelled_{};
};
// One common pre-batch hook. The game owns framing/compression/encryption;
// edits substitute a temporary string and never modify game-owned allocation.
class PacketWireEvents {
public:
    void load(const nlohmann::json& root) {
        const auto& wire=root.at("layouts").at("PacketWire");
        if(wire.value("schema",0)!=1||wire.value("pointer_size",0)!=8||
           wire.value("format",std::string{})!="bedrock_packet_header_and_payload"||
           wire.value("protocol_schema",std::string{})!="movement_array_options_v1"||
           wire.value("abi",std::string{})!="void(stage*, const std::string&, uintptr_t, int)")
            throw std::runtime_error("unsupported common packet wire bridge");
        const auto& function=wire.at("batch_append");
        target_=reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))+native_profile::number(function.at("rva")));
        if(!BDS_HookOwnsTarget(BDS_HOOK_PACKET_WIRE_SEND,target_))target_=native_profile::checked(root,function);
        DWORD64 image{};const auto* entry=RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(target_),&image,nullptr);
        if(!entry||image+entry->BeginAddress!=reinterpret_cast<DWORD64>(target_))throw std::runtime_error("wire bridge is not a function entry");
        if(!BDS_HookRuntimeSupports(BDS_HOOK_PACKET_WIRE_SEND))
            throw std::runtime_error("Packet wire Hook requires runtime 4.6 or newer; replace DLLs and restart Minecraft");
    }
    bool subscribe(BDS_HookCallback callback,void* user,BDS_HookToken* token) const {
        return target_&&BDS_HookRuntimeSupports(BDS_HOOK_PACKET_WIRE_SEND)&&BDS_HookSubscribe(BDS_HOOK_PACKET_WIRE_SEND,target_,callback,user,token);
    }
    // false means preparation/handler failed and the continuation was NOT run.
    template<class Handler> bool dispatch(BDS_PacketWireArgs& args,Handler&& handler,BDS_HookNext next,void* frame) const {
        // This bridge is the release MSVC x64 std::string ABI shared by all
        // packets. Copy through bounded reads before executing Mod code.
        std::optional<WirePacket> packet;bool cancelled{};
        try {
            std::string copy;if(!copyString(args.bytes,copy))return false;
            packet.emplace(std::move(copy));PacketWireSendEvent event(args.sender,*packet);
            handler(event);cancelled=event.cancelled();
        }catch(...){return false;}
        if(!cancelled){auto edited=args;if(packet->modified())edited.bytes=&packet->bytes();next(frame,&edited);}
        return true;
    }
private:
    static bool copyBytes(const void* source,void* destination,std::size_t size) noexcept {
        __try{std::memcpy(destination,source,size);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    static bool copyString(const void* object,std::string& result) {
        std::array<std::uintptr_t,4> value{};
        if(!native_profile::read(object,0,value)||value[2]>value[3]||value[2]>WirePacket::maximumBytes)return false;
        const void* bytes=value[3]<16?object:reinterpret_cast<const void*>(value[0]);
        if(!bytes)return false;result.resize(value[2]);return copyBytes(bytes,result.data(),result.size());
    }
    void* target_{};
};
} // namespace bedrock
