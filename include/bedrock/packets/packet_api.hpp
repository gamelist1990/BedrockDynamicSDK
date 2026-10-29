#pragma once
#include <bedrock/packets/packet_read_events.hpp>
#include <bedrock/packets/wire_packet_events.hpp>
#include <atomic>
#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace bedrock {
// Mod-facing filtered packet subscriptions. Receive runs after successful game
// decoding and borrows its copied payload for this callback only. Send runs
// before batching and may edit event.packet() or cancel the packet.
// Callbacks may be on networking threads: retain copies, never native Actors.
class PacketApi {
public:
    using Subscription=std::uint64_t;
    using ReceiveHandler=std::function<void(const PacketReadEvent&)>;
    using SendHandler=std::function<void(PacketWireSendEvent&)>;
    Subscription onReceive(PacketId id,ReceiveHandler handler){return add(static_cast<int>(id),std::move(handler),{});}
    Subscription onAnyReceive(ReceiveHandler handler){return add(-1,std::move(handler),{});}
    Subscription onSend(PacketId id,SendHandler handler){return add(static_cast<int>(id),{},std::move(handler));}
    Subscription onAnySend(SendHandler handler){return add(-1,{},std::move(handler));}
    void unsubscribe(Subscription id){
        std::lock_guard lock(mutex_);auto next=std::make_shared<std::vector<Entry>>(*handlers_.load());
        std::erase_if(*next,[&](const Entry& e){return e.token==id;});handlers_.store(std::move(next));
    }
    bool installReceive(std::string_view selected,std::string_view embedded,std::string& error){
        try{return installReceive(native_profile::layoutProfile(selected,embedded,"PacketRead"),error);}
        catch(const std::exception& e){error=e.what();return false;}
    }
    bool installReceive(std::string_view profile,std::string& error){
        if(receiveToken_)return true;
        try{reads_.load(parseNativeProfile(profile));
            if(!reads_.subscribe(&receive,this,&receiveToken_))throw std::runtime_error("Packet receive subscription failed");
            receiveReady_.store(true);error.clear();return true;}catch(const std::exception& e){error=e.what();return false;}
    }
    bool installSend(std::string_view profile,std::string& error){
        if(sendToken_)return true;
        try{sends_.load(parseNativeProfile(profile));
            if(!sends_.subscribe(&send,this,&sendToken_))throw std::runtime_error("Packet send subscription failed");
            error.clear();return true;}catch(const std::exception& e){error=e.what();return false;}
    }
    bool receiveReady() const noexcept{return receiveReady_.load();}
    // Explicit stop outside DllMain. An in-flight callback may finish; owners
    // must retain this object until callbacks no longer use it (Mods pin it).
    void stop(){
        receiveReady_.store(false);
        {std::lock_guard lock(mutex_);handlers_.store(std::make_shared<const std::vector<Entry>>());}
        if(receiveToken_)BDS_HookUnsubscribe(receiveToken_);receiveToken_=nullptr;
        if(sendToken_)BDS_HookUnsubscribe(sendToken_);sendToken_=nullptr;
    }
private:
    struct Entry {Subscription token{};int id{-1};ReceiveHandler receive;SendHandler send;};
    Subscription add(int id,ReceiveHandler receive,SendHandler send){
        std::lock_guard lock(mutex_);auto next=std::make_shared<std::vector<Entry>>(*handlers_.load());
        const auto token=++serial_;next->push_back({token,id,std::move(receive),std::move(send)});handlers_.store(std::move(next));return token;
    }
    static void BDS_HOOK_CALL receive(void* user,void* raw,BDS_HookNext next,void* frame){
        auto& self=*static_cast<PacketApi*>(user);
        const auto handlers=self.handlers_.load();
        self.reads_.dispatchFiltered(*static_cast<BDS_PacketReadArgs*>(raw),[&](const PacketReadEvent& event){
            for(const auto& entry:*handlers)
                if(entry.receive&&(entry.id<0||entry.id==static_cast<int>(event.rawId())))try{entry.receive(event);}catch(...){}
        },next,frame,[&](unsigned id){return std::any_of(handlers->begin(),handlers->end(),[&](const Entry& e){return e.receive&&(e.id<0||e.id==static_cast<int>(id));});});
    }
    static void BDS_HOOK_CALL send(void* user,void* raw,BDS_HookNext next,void* frame){
        auto& self=*static_cast<PacketApi*>(user);auto& args=*static_cast<BDS_PacketWireArgs*>(raw);
        if(!self.sends_.dispatch(args,[&](PacketWireSendEvent& event){
            const auto handlers=self.handlers_.load();for(const auto& entry:*handlers)
                if(entry.send&&(entry.id<0||entry.id==static_cast<int>(event.id())))try{entry.send(event);}catch(...){}
        },next,frame))next(frame,&args);
    }
    PacketReadEvents reads_;PacketWireEvents sends_;BDS_HookToken receiveToken_{},sendToken_{};
    std::atomic<bool> receiveReady_{};
    std::mutex mutex_;Subscription serial_{};
    std::atomic<std::shared_ptr<const std::vector<Entry>>> handlers_{std::make_shared<const std::vector<Entry>>()};
};
} // namespace bedrock
