#pragma once
#include "native.hpp"
#include <array>
#include <atomic>
#include <string>

namespace top_native {
// Windows x64 Command layout corroborated by the exact-build Help factory.
// Instances belong to this pinned DLL; the virtual deleting destructor also
// stays here, so game-owned unique_ptrs never free them with another allocator.
class NativeTopCommand;
using CommandReceiver=void(*)(const NativeTopCommand&,void* output);
inline CommandReceiver commandReceiver{};
class NativeTopCommand {
public:
    virtual ~NativeTopCommand()=default;
    virtual bool collectOptionalArguments(){return true;}
    virtual void execute(const void*,void* output)const{if(commandReceiver)commandReceiver(*this,output);}
    int version{};std::uint32_t padding{};
    const void* registry{};
    int symbol{-1};std::uint8_t permission{5},reserved{};std::uint16_t flags{};
    int blocks{};bool hasBlocks{};
};
static_assert(sizeof(NativeTopCommand)==0x28);
inline void* __fastcall createTopCommand(void* result){
    auto* value=new NativeTopCommand;
    std::memcpy(result,&value,sizeof(value));return result;
}
class NativeCommands {
    const Bindings& b;
    void* registerFn{};void* findFn{};void* parameterFn{};void* overloadFn{};
    void* factoryTable{};void* integerType{};void* integerRule{};
    bool ready{};
    std::atomic<const void*> currentRegistry{};
    void* failedRegistry{};
    std::string error_;
    static void* find(void* fn,void* registry,const std::string& name)noexcept{
        __try{return reinterpret_cast<void*(__fastcall*)(void*,const std::string*)>(fn)(registry,&name);}__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
    }
    bool ours(void* signature)const noexcept{
        void* first{};void* last{};void* table{};void* function{};
        return n::read(signature,0x40,first)&&n::read(signature,0x48,last)&&first&&last&&
            reinterpret_cast<std::uintptr_t>(last)>=reinterpret_cast<std::uintptr_t>(first)+0x80&&
            n::read(first,8,table)&&table==factoryTable&&n::read(first,16,function)&&function==reinterpret_cast<void*>(&createTopCommand);
    }
    bool registerOne(void* registry,const std::string& name)noexcept{
        alignas(8) std::array<std::byte,0x78> parameter{};
        // The native small callable stores a vtable and a free function, inside
        // 64-byte storage. Its verified native copy/move thunks only copy +8;
        // its destructor is a no-op. Do not substitute std::function here.
        alignas(8) std::array<std::uintptr_t,8> factory{};
        factory[0]=reinterpret_cast<std::uintptr_t>(factoryTable);
        factory[1]=reinterpret_cast<std::uintptr_t>(&createTopCommand);
        __try{
            // Native parameter name is short SSO (6 bytes), so this temporary
            // owns no heap allocation. The native helper deep-copies it using
            // the game's own allocator when it builds the overload vector.
            reinterpret_cast<void*(__fastcall*)(void*,const void*,const void*,const char*,int,const char*,const char*,int,bool,int)>(parameterFn)(
                parameter.data(),integerType,integerRule,"blocks",0,nullptr,nullptr,0x20,true,0x24);
            std::size_t size{},capacity{};
            if(!n::read(parameter.data(),0x28,size)||!n::read(parameter.data(),0x30,capacity)||size!=6||capacity!=15)return false;
            reinterpret_cast<void(__fastcall*)(void*,const std::string*,const char*,unsigned char,unsigned short,unsigned short)>(registerFn)(
                registry,&name,"安全な上方移動 / safe upward teleport",0,0x10,0x80); // Any, SyncLocal, NotCheat: same flags as native help.
            const auto version=std::uint64_t{0x7fffffff00000001};
            return reinterpret_cast<void*(__fastcall*)(void*,const char*,std::uint64_t,void*,const void*)>(overloadFn)(
                registry,"top",version,factory.data(),parameter.data())!=nullptr;
        }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
public:
    enum class Result {unavailable,waiting,unchanged,registered,collision,failed};
    explicit NativeCommands(const Bindings& bindings):b(bindings){}
    bool load(const std::string& profile,std::string& error){
        try{
            const auto j=nlohmann::json::parse(profile);const auto& c=j.at("native_top").at("commands");
            if(c.at("schema")!=1)throw std::runtime_error("unsupported native command schema");
            registerFn=b.checked(c.at("register_command"));findFn=b.checked(c.at("find_command"));
            parameterFn=b.checked(c.at("make_parameter"));overloadFn=b.checked(c.at("register_optional_parameter"));
            factoryTable=reinterpret_cast<void*>(b.base+n::number(c.at("factory_vtable_rva")));
            const auto copy=b.checked(c.at("factory_copy")),destroy=b.checked(c.at("factory_destroy")),invoke=b.checked(c.at("factory_invoke"));
            std::array<void*,4> actual{};
            if(!n::read(factoryTable,0,actual)||actual!=std::array<void*,4>{copy,destroy,invoke,copy})throw std::runtime_error("native factory vtable mismatch");
            integerType=reinterpret_cast<void*>(b.base+n::number(c.at("integer_type_rva")));
            integerRule=reinterpret_cast<void*>(b.base+n::number(c.at("integer_rule_rva")));
            b.checked(c.at("client_commands_anchor"));b.checked(c.at("has_commands_anchor"));
            ready=true;error.clear();return true;
        }catch(const std::exception& e){ready=false;error=e.what();return false;}
    }
    void reset()noexcept{currentRegistry.store(nullptr);}
    const void* registry()const noexcept{return currentRegistry.load();}
    const std::string& error()const noexcept{return error_;}
    // Call only on the owning client tick, never on a worker or native read hook.
    Result update(void* client){
        if(!ready)return Result::unavailable;
        void* minecraft{};void* commands{};void* registry{};void* parser{};
        std::uint16_t type{};
        if(!n::read(client,0x1b0,minecraft)||!n::read(minecraft,0xc0,commands)||!n::read(commands,0x10,registry)||!registry||
           !n::read(integerType,0,type)||type>4096||!n::read(integerRule,0,parser)||!b.executable(parser))return Result::waiting;
        const std::string name="top";
        if(auto* signature=find(findFn,registry,name)){
            if(ours(signature)){currentRegistry.store(registry);return Result::unchanged;}
            currentRegistry.store(nullptr);return Result::collision;
        }
        currentRegistry.store(nullptr);
        if(failedRegistry==registry)return Result::failed;
        if(!registerOne(registry,name)){failedRegistry=registry;error_="native command registration failed";return Result::failed;}
        auto* signature=find(findFn,registry,name);
        if(!signature||!ours(signature)){failedRegistry=registry;error_="native overload could not be confirmed";return Result::failed;}
        currentRegistry.store(registry);return Result::registered;
    }
    bool markAccepted(void* output)const noexcept{
        unsigned count{};return n::read(output,0x28,count)&&count<UINT32_MAX&&n::write(output,0x28,count+1);
    }
};
}
