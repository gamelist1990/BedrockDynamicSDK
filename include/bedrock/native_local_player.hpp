#pragma once
#include <bedrock/detail/native_profile_functions.hpp>

namespace bedrock {
// Prefer the exact-build getter over an inferred ClientInstance vtable slot.
class NativeLocalPlayerGetter {
public:
    void load(const nlohmann::json& root) {
        target_=nullptr;
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        begin_=base+native_profile::number(root.at("target").at("text_rva"));
        end_=begin_+native_profile::number(root.at("target").at("text_size"));
        if(root.contains("native_camera")&&root.at("native_camera").contains("local_player"))
            target_=native_profile::checked(root,root.at("native_camera").at("local_player"));
    }
    void* get(void* client,std::size_t legacySlot) const noexcept {
        if(!client)return nullptr;
        void* target=target_;
        if(!target){
            void* table{};
            if(legacySlot>0x1000||!native_profile::read(client,0,table)||!native_profile::read(table,legacySlot*8,target))return nullptr;
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
    void* target_{};std::uintptr_t begin_{},end_{};
};
} // namespace bedrock
