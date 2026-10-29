#pragma once
#include <bedrock/packet_ids.hpp>
#include <bedrock/detail/native_profile_functions.hpp>

namespace bedrock {
class NativePacketView;
// The ABI of Packet's common virtual interface is versioned once, rather
// than storing one getId/isValid address and one vtable RVA for each packet.
class NativePacketAccess {
public:
    void load(const nlohmann::json& root) {
        const auto& abi=root.at("layouts").at("Packet");
        if(abi.value("schema",0)!=1 || abi.value("pointer_size",0)!=8 ||
           native_profile::number(abi.at("get_id_slot"))!=1 || native_profile::number(abi.at("is_valid_slot"))!=11)
            throw std::runtime_error("unsupported native Packet virtual ABI");
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        begin_=base+native_profile::number(root.at("target").at("text_rva"));
        end_=begin_+native_profile::number(root.at("target").at("text_size"));
    }
    NativePacketView view(void* packet) const noexcept;
private:
    friend class NativePacketView;
    void* method(const void* packet,std::size_t slot) const noexcept {
        void *table{},*target{};
        if(!native_profile::read(packet,0,table)||!native_profile::read(table,slot*8,target))return nullptr;
        const auto address=reinterpret_cast<std::uintptr_t>(target);
        return address>=begin_&&address<end_ ? target : nullptr;
    }
    std::uintptr_t begin_{},end_{};
};
class NativePacketView {
public:
    PacketId id() const noexcept {return id_;}
    int rawId() const noexcept {return rawId_;}
    void* native() const noexcept {return packet_;}
    bool valid() const noexcept {
        auto* fn=access_->method(packet_,11);if(!fn)return false;
        __try{return reinterpret_cast<bool(__fastcall*)(const void*)>(fn)(packet_);}
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
private:
    friend class NativePacketAccess;
    NativePacketView(const NativePacketAccess* access,void* packet) noexcept:access_(access),packet_(packet) {
        rawId_=readId(access_->method(packet_,1),packet_);
        if(rawId_>=0)id_=packetIdFromValue(static_cast<unsigned>(rawId_)).value_or(PacketId::Unknown);
    }
    static int readId(void* fn,void* packet) noexcept {
        if(!fn)return -1;
        __try{const auto value=reinterpret_cast<unsigned(__fastcall*)(const void*)>(fn)(packet);return value<=0x3ff ? static_cast<int>(value) : -1;}
        __except(EXCEPTION_EXECUTE_HANDLER){return -1;}
    }
    const NativePacketAccess* access_{};void* packet_{};
    int rawId_{-1};PacketId id_{PacketId::Unknown};
};
inline NativePacketView NativePacketAccess::view(void* packet) const noexcept {return NativePacketView(this,packet);}
}
