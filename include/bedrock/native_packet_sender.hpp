#pragma once
#include <bedrock/detail/native_profile_functions.hpp>

namespace bedrock {
// Borrow only on the owning game callback. This path is shared by command
// requests and movement interception; resolving it never sends a packet.
class NativePacketSender {
public:
    void load(const nlohmann::json& root) {
        const auto& sender=root.at("native_packets").at("sender");
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        textBegin_=base+native_profile::number(root.at("target").at("text_rva"));
        textEnd_=textBegin_+native_profile::number(root.at("target").at("text_size"));
        slot_=native_profile::number(sender.at("sender_send_to_server_slot"));
        commands_=sender.value("path",std::string{})=="MinecraftCommands::client->getPacketSender";
        if(commands_) {
            if(root.at("target").at("sha256")!="988f6a61403b4381aa41e6c66d64cce441b14e561a9b10f647c2758ef3f2672a" || slot_!=2)
                throw std::runtime_error("unsupported MinecraftCommands sender path");
            native_profile::checked(root,sender.at("command_request_path"));
        } else {
            offset_=native_profile::number(sender.at("client_packet_sender_offset"));
            if(offset_!=0x1d0 || slot_!=4) throw std::runtime_error("unsupported legacy PacketSender layout");
            native_profile::checked(root,sender.at("packet_sender_getter"));
        }
    }
    bool resolve(void* client,void*& sender,void*& target) const noexcept {
        sender=nullptr;target=nullptr;void* table{};
        if(commands_) {
            void *minecraft{},*commands{},*provider{},*owner{};
            if(!native_profile::read(client,0x1d0,minecraft)||!minecraft||
               !native_profile::read(minecraft,0xc8,commands)||!commands||
               !native_profile::read(commands,8,provider)||!provider||
               !getter(provider,8,owner)||!owner||!getter(owner,0xa40,sender)||!sender) return false;
        } else if(!native_profile::read(client,offset_,sender)||!sender) return false;
        if(!native_profile::read(sender,0,table)||!native_profile::read(table,slot_*8,target))return false;
        const auto address=reinterpret_cast<std::uintptr_t>(target);
        return address>=textBegin_&&address<textEnd_;
    }
private:
    bool getter(void* object,std::size_t offset,void*& value) const noexcept {
        void *table{},*fn{};
        if(!native_profile::read(object,0,table)||!native_profile::read(table,offset,fn))return false;
        const auto address=reinterpret_cast<std::uintptr_t>(fn);
        if(address<textBegin_||address>=textEnd_)return false;
        __try {value=reinterpret_cast<void*(__fastcall*)(void*)>(fn)(object);return true;}
        __except(EXCEPTION_EXECUTE_HANDLER){value=nullptr;return false;}
    }
    std::uintptr_t textBegin_{},textEnd_{};
    std::uint32_t offset_{},slot_{};
    bool commands_{};
};
}
