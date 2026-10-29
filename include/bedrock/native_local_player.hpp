#pragma once
#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/profile_json.hpp>

namespace bedrock {
// Prefer the exact-build getter over an inferred ClientInstance vtable slot.
class NativeLocalPlayerGetter {
public:
    void load(const std::string& profile){load(std::string_view(profile));}
    void load(std::string_view profile){load(parseNativeProfile(profile));}
    void load(const nlohmann::json& root) {
        target_=nullptr;legacySlot_=0;
        if(root.contains("native_items"))legacySlot_=native_profile::number(root.at("native_items").at("local_player").at("get_local_player_vtable_slot"));
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        begin_=base+native_profile::number(root.at("target").at("text_rva"));
        end_=begin_+native_profile::number(root.at("target").at("text_size"));
        if(root.contains("symbols")&&root.at("symbols").contains("ClientInstance::getLocalPlayer"))
            target_=native_profile::checked(root,root.at("symbols").at("ClientInstance::getLocalPlayer"));
        else if(root.contains("native_camera")&&root.at("native_camera").contains("local_player"))
            target_=native_profile::checked(root,root.at("native_camera").at("local_player"));
    }
    void* get(void* client) const noexcept {
        if(!client)return nullptr;
        void* target=target_;
        if(!target){
            void* table{};
            if(!legacySlot_||legacySlot_>0x1000||!native_profile::read(client,0,table)||!native_profile::read(table,legacySlot_*8,target))return nullptr;
            const auto address=reinterpret_cast<std::uintptr_t>(target);
            if(address<begin_||address>=end_)return nullptr;
        }
        return invoke(target,client);
    }
    bool exactGetter() const noexcept{return target_!=nullptr;}
private:
    static void* invoke(void* target,void* client) noexcept {
        __try{return reinterpret_cast<void*(__fastcall*)(void*)>(target)(client);}
        __except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
    }
    void* target_{};std::uintptr_t begin_{},end_{};std::size_t legacySlot_{};
};
} // namespace bedrock
