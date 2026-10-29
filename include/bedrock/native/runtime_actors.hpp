#pragma once
#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/profiles/profile_json.hpp>
#include <optional>
#include <vector>
namespace bedrock {
// Invoke on the owning Level tick only. Returned handles are not owned and
// must be validated/consumed on that tick, never retained by a worker.
class NativeRuntimeActors {
public:
    bool load(std::string_view selected,std::string_view embedded,std::string& error){
        try{return load(native_profile::layoutProfile(selected,embedded,"RuntimeActorList"),error);}
        catch(const std::exception& e){error=e.what();return false;}
    }
    bool load(std::string_view profile,std::string& error){
        target_=nullptr;
        try{
            const auto root=parseNativeProfile(profile);const auto& layout=root.at("layouts").at("RuntimeActorList");
            if(layout.at("schema")!=1||layout.at("abi")!="void(Level*, std::vector<Actor*>* hidden_return)")
                throw std::runtime_error("unsupported runtime Actor list ABI");
            limit_=native_profile::number(layout.at("maximum_actors"));
            if(!limit_||limit_>65536)throw std::runtime_error("invalid runtime Actor list bound");
            target_=native_profile::checked(root,layout.at("function"));error.clear();return true;
        }catch(const std::exception& e){error=e.what();return false;}
    }
    std::optional<std::vector<void*>> snapshot(void* level) const {
        if(!level||!target_)return std::nullopt;
        std::vector<void*> result;if(!invoke(level,&result)||result.size()>limit_)return std::nullopt;return result;
    }
    bool ready() const noexcept{return target_!=nullptr;}
private:
    bool invoke(void* level,std::vector<void*>* output) const noexcept {
        __try{reinterpret_cast<void(__fastcall*)(void*,std::vector<void*>*)>(target_)(level,output);return true;}
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    void* target_{};std::size_t limit_{};
};
} // namespace bedrock
