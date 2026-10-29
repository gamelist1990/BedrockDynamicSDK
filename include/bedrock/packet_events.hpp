#pragma once
#include <bedrock/native_packet_view.hpp>
#include <bedrock/hook_runtime.h>

namespace bedrock {
// A synchronous borrowed event. Changes must finish before the native send
// continuation; only copied values may be queued for another game tick.
class PacketSendEvent {
public:
    PacketSendEvent(void* sender,NativePacketView packet):sender_(sender),packet_(packet){}
    void* sender() const noexcept{return sender_;}
    PacketId id() const noexcept{return packet_.id();}
    NativePacketView packet() const noexcept{return packet_;}
    void cancel() noexcept{cancelled_=true;}
    bool cancelled() const noexcept{return cancelled_;}
private:
    void* sender_{};NativePacketView packet_;bool cancelled_{};
};
// Every Mod uses the same BDS_HOOK_PACKET_SEND_TO_SERVER subscription point.
// The process-wide hook runtime installs the native hook once and chains Mods.
class NativePacketEvents {
public:
    void load(const nlohmann::json& root){access_.load(root);}
    NativePacketView packet(void* value) const noexcept{return access_.view(value);}
    template<class Handler> void dispatch(BDS_PacketSendArgs& args,Handler&& handler,BDS_HookNext next,void* frame) const noexcept {
        PacketSendEvent event(args.sender,access_.view(args.packet));
        try{handler(event);}catch(...){event.cancel();}
        if(!event.cancelled())next(frame,&args);
    }
private:
    NativePacketAccess access_;
};
}
