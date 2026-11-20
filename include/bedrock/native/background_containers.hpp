#pragma once
#include <bedrock/detail/block_types.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/packets/protocol_codec.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <array>
#include <atomic>
#include <mutex>
#include <cstring>

namespace bedrock {
// Server container ownership without pushing a client screen. Network callbacks
// only capture ownership; sends occur from the owner's game-thread pump.
class NativeBackgroundContainers {
public:
    enum class Kind { Inventory, Table, Furnace };
    ~NativeBackgroundContainers() { stop(); }
    bool load(const nlohmann::json& root,std::string& error) {
        try {
            layout_=root.at("layouts").at("BackgroundContainers");
            if (layout_.at("schema")!=1 || layout_.at("revision")!=1 || layout_.at("pointer_size")!=8)
                throw std::runtime_error("unsupported background container layout");
            if (number("interact_size")!=88 || number("close_size")!=56)
                throw std::runtime_error("unsupported background packet sizes");
            base_=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            openTarget_=native_profile::checked(root,layout_.at("open_dispatch"));
            closeTarget_=native_profile::checked(root,layout_.at("close_dispatch"));
            for (const auto* name:{"interact_id_guard","interact_write_guard","close_id_guard","close_write_guard"})
                (void)native_profile::checked(root,layout_.at(name));
            for (const auto& [key,value]:layout_.items())
                if (key.ends_with("_offset") && native_profile::number(value)>88)
                    throw std::runtime_error("background packet field outside bound");
            // Protocol values come from the embedded, selected protocol schema.
            const auto& types=ProtocolCodec::types();
            for (const auto& [key,value]:types.at("WindowType").at(1).at("mappings").items()) {
                if (value=="inventory") inventoryType_=std::stoi(key);
                if (value=="workbench") tableType_=std::stoi(key);
                if (value=="furnace") furnaceType_=std::stoi(key);
            }
            const auto& action=types.at("packet_interact").at(1).at(0).at("type").at(1).at("mappings");
            for (const auto& [key,value]:action.items()) if (value=="open_inventory") openInventoryAction_=std::stoi(key);
            if (inventoryType_==-999 || tableType_==-999 || furnaceType_==-999 || openInventoryAction_<0)
                throw std::runtime_error("background container protocol mappings missing");
            loaded_=true;error.clear();return true;
        } catch (const std::exception& e) { error=e.what();return false; }
    }
    void context(void* sender,void* connection,bool allowWork=true) {
        sender_=sender;connection_=connection;workAllowed_=allowWork;
        if (workAllowed_) pump();
    }
    bool arm(Kind kind,BlockPos position={}) {
        if (!workAllowed_ || !loaded_ || !sender_ || !connection_ || !install()) return false;
        std::lock_guard lock(mutex_);
        if (state_!=State::Closed) return kind_==kind && state_!=State::Failed && state_!=State::Closing &&
            (kind==Kind::Inventory || (position_.x==position.x && position_.y==position.y && position_.z==position.z));
        kind_=kind;position_=position;type_=typeOf(kind);window_=-1;callback_=nullptr;
        state_=State::Opening;deadline_=GetTickCount64()+5000;return true;
    }
    bool openInventory(std::uint64_t runtime) {
        if (!runtime) return false;
        if (active()) return arm(Kind::Inventory);
        if (!arm(Kind::Inventory)) return false;
        std::array<std::byte,88> packet{};init(packet.data(),"interact");
        put(packet.data(),"interact_action_offset",static_cast<unsigned char>(openInventoryAction_));
        put(packet.data(),"interact_runtime_offset",runtime);
        put(packet.data(),"interact_version_offset",number("interact_version"));
        dispatch(packet.data());return true;
    }
    bool isOpen(Kind kind) const { std::lock_guard lock(mutex_);return state_==State::Open && kind_==kind; }
    bool active() const { std::lock_guard lock(mutex_);return state_!=State::Closed; }
    bool failed() const { std::lock_guard lock(mutex_);return state_==State::Failed; }
    // Returns true only after the matching server close, or after a bounded
    // timeout. Never sends Esc or closes a user's client screen.
    bool close() {
        { std::lock_guard lock(mutex_);
          if (state_==State::Closed) return true;
          if (state_==State::Opening) { state_=State::Closing;deadline_=GetTickCount64()+5000; }
          else if (state_==State::Open || state_==State::Failed) { state_=State::Closing;closeSent_=false;deadline_=GetTickCount64()+3000; }
        }
        if (workAllowed_) pump();
        return !active();
    }
    void stop() { auto* expected=this;shared().owner.compare_exchange_strong(expected,nullptr); }
    void reset() { std::lock_guard lock(mutex_);state_=State::Closed;window_=-1;callback_=nullptr;closeSent_=false; }
private:
    enum class State { Closed, Opening, Open, Closing, Failed };
    using Handler=void(__fastcall*)(void*,void*,void*,const void*);
    struct Shared {
        std::mutex mutex;std::atomic<NativeBackgroundContainers*> owner{};
        Handler open{},close{};void* openTarget{};void* closeTarget{};
        BDS_HookToken openToken{},closeToken{};
    };
    static Shared& shared() { static auto* s=new Shared;return *s; }
    bool install() {
        auto& s=shared();std::lock_guard lock(s.mutex);
        if (s.owner.load() && s.owner.load()!=this) return false;
        if ((s.openToken && s.openTarget!=openTarget_) || (s.closeToken && s.closeTarget!=closeTarget_)) return false;
        if (!s.openToken) {
            if (!BDS_HookDirectInstall(openTarget_,reinterpret_cast<void*>(&onOpen),reinterpret_cast<void**>(&s.open),&s.openToken)) return false;
            s.openTarget=openTarget_;
            HMODULE module{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&onOpen),&module);
        }
        if (!s.closeToken) {
            if (!BDS_HookDirectInstall(closeTarget_,reinterpret_cast<void*>(&onClose),reinterpret_cast<void**>(&s.close),&s.closeToken)) return false;
            s.closeTarget=closeTarget_;
        }
        s.owner.store(this);return true;
    }
    static void __fastcall onOpen(void* dispatcher,void* network,void* callback,const void* ref) {
        auto& s=shared();auto* self=s.owner.load();
        if (self && self->receiveOpen(callback,ref)) return;
        s.open(dispatcher,network,callback,ref);
    }
    static void __fastcall onClose(void* dispatcher,void* network,void* callback,const void* ref) {
        auto& s=shared();auto* self=s.owner.load();
        if (self && self->receiveClose(callback,ref)) return;
        s.close(dispatcher,network,callback,ref);
    }
    bool receiveOpen(void* callback,const void* ref) {
        void* packet{};void* vt{};unsigned char id{};signed char type{};BlockPos position{};
        if (!native_profile::read(ref,0,packet) || !native_profile::read(packet,0,vt) ||
            reinterpret_cast<std::uintptr_t>(vt)!=base_+number("open_vtable_rva") ||
            !native_profile::read(packet,number("open_id_offset"),id) ||
            !native_profile::read(packet,number("open_type_offset"),type) ||
            !native_profile::read(packet,number("open_position_offset"),position)) return false;
        std::lock_guard lock(mutex_);
        if (state_==State::Closed || type!=type_ || id==255) return false;
        if (kind_!=Kind::Inventory && (position.x!=position_.x || position.y!=position_.y || position.z!=position_.z)) return false;
        // Repeated responses for the owned window must never reach the screen
        // dispatcher, including while its close acknowledgement is pending.
        if (window_>=0) return id==window_ && callback==callback_;
        window_=id;callback_=callback;closeSent_=false;
        if (state_==State::Opening) state_=State::Open;
        else { state_=State::Closing;deadline_=GetTickCount64()+3000; }
        return true;
    }
    bool receiveClose(void* callback,const void* ref) {
        void* packet{};unsigned char id{};
        if (!native_profile::read(ref,0,packet) || !native_profile::read(packet,number("close_id_offset"),id)) return false;
        std::lock_guard lock(mutex_);
        if (window_<0 || id!=window_ || callback!=callback_) return false;
        state_=State::Closed;window_=-1;closeSent_=false;return true;
    }
    void pump() {
        int window=-1,type=0;
        { std::lock_guard lock(mutex_);const auto now=GetTickCount64();
          if (state_==State::Opening && now>deadline_) { state_=State::Failed;deadline_=now+5000; }
          if ((state_==State::Closing || state_==State::Failed) && now>deadline_) { state_=State::Closed;window_=-1; }
          if (state_==State::Closing && window_>=0 && !closeSent_) { window=window_;type=type_;closeSent_=true; }
        }
        if (window>=0 && sender_ && connection_) {
            std::array<std::byte,88> packet{};init(packet.data(),"close");
            put(packet.data(),"close_id_offset",static_cast<unsigned char>(window));
            put(packet.data(),"close_type_offset",static_cast<signed char>(type));
            put(packet.data(),"close_version_offset",number("close_version"));dispatch(packet.data());
        }
    }
    std::uint32_t number(const char* key) const { return native_profile::number(layout_.at(key)); }
    template<class T> void put(std::byte* packet,const char* key,const T& value) { std::memcpy(packet+number(key),&value,sizeof(value)); }
    void init(std::byte* packet,const char* kind) {
        const auto table=base_+native_profile::number(layout_.at(std::string(kind)+"_vtable_rva"));
        std::memcpy(packet,&table,8);const auto priority=layout_.at("packet_priority").get<std::uint64_t>();
        put(packet,"packet_priority_offset",priority);put(packet,"packet_connection_offset",connection_);
    }
    void dispatch(void* packet) { BDS_PacketSendArgs args{sender_,packet};BDS_HookDispatch(BDS_HOOK_PACKET_SEND_TO_SERVER,&args); }
    int typeOf(Kind k) const { return k==Kind::Inventory?inventoryType_:k==Kind::Table?tableType_:furnaceType_; }
    nlohmann::json layout_;std::uintptr_t base_{};void* openTarget_{};void* closeTarget_{};
    bool loaded_{},workAllowed_{};void* sender_{};void* connection_{};
    mutable std::mutex mutex_;State state_{State::Closed};Kind kind_{};BlockPos position_{};
    int type_{},window_{-1};void* callback_{};ULONGLONG deadline_{};bool closeSent_{};
    int inventoryType_{-999},tableType_{-999},furnaceType_{-999},openInventoryAction_{-1};
};
}
