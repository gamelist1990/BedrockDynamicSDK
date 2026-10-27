#pragma once
#if !defined(_WIN32) || !defined(_MSC_VER) || !defined(_WIN64)
#error "Native command registration requires Windows x64 MSVC"
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bedrock/resolver.hpp>
#include <bedrock/command_builder.hpp>
#include <bedrock/profile_manager.hpp>
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bedrock {
struct NativeCommandInvocation { std::string name; std::optional<int> integer; };
struct CommandIntegerArgument { std::string name="value"; bool optional=true; };
namespace command_native {
template<class T>bool read(const void* p,std::size_t offset,T& out)noexcept{
    if(!p)return false;__try{std::memcpy(&out,static_cast<const std::byte*>(p)+offset,sizeof(T));return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
template<class T>bool write(void* p,std::size_t offset,const T& value)noexcept{
    if(!p)return false;__try{std::memcpy(static_cast<std::byte*>(p)+offset,&value,sizeof(T));return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
struct Route {
    std::string name,description;
    CommandIntegerArgument parameter;
    std::function<bool(const NativeCommandInvocation&)> handler;
    std::atomic<const void*> registry{};
    void* failedRegistry{}; // owning tick only
};
inline constexpr std::size_t maxCommands=32;
// Slots are never recycled: previously constructed native commands must never
// execute a new handler after a manager is destroyed or re-created.
inline std::array<std::atomic<std::shared_ptr<Route>>,maxCommands> routes;
inline std::atomic<std::size_t> nextSlot{};
inline bool acceptOutput(void* output)noexcept{
    unsigned count{};return read(output,0x28,count)&&count<UINT32_MAX&&write(output,0x28,count+1);
}
class Command {
public:
    explicit Command(std::size_t slot):routeIndex(slot){}
    virtual ~Command()=default;
    virtual bool collectOptionalArguments(){return true;}
    virtual void execute(const void*,void* output)const{
        try{
            if(routeIndex>=routes.size())return;
            const auto route=routes[routeIndex].load(std::memory_order_acquire);
            if(!route||!registry||registry!=route->registry.load(std::memory_order_acquire))return;
            const NativeCommandInvocation invocation{route->name,hasArgument?std::optional<int>{argument}:std::nullopt};
            if(route->handler(invocation))acceptOutput(output);
        }catch(...){/* exceptions must not cross the native command ABI */}
    }
    int version{};std::uint32_t padding{};
    const void* registry{};
    int symbol{-1};std::uint8_t permission{5},reserved{};std::uint16_t flags{};
    int argument{};bool hasArgument{};
    std::size_t routeIndex{};
};
static_assert(sizeof(Command)==0x30);
using Factory=void*(__fastcall*)(void*);
template<std::size_t Index>void* __fastcall create(void* result){
    Command* value=new(std::nothrow) Command(Index);
    std::memcpy(result,&value,sizeof(value));return result;
}
template<std::size_t... I>constexpr auto factories(std::index_sequence<I...>){return std::array<Factory,sizeof...(I)>{&create<I>...};}
inline constexpr auto factoryFunctions=factories(std::make_index_sequence<maxCommands>{});
inline bool nameValid(std::string_view name){
    if(name.empty()||name.size()>64)return false;
    for(const unsigned char c:name)if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return false;
    return true;
}
}
// Register definitions before callbacks start. load()/update() belong to the
// initialization/owning client tick; reset() may be called by a world event.
// Native callbacks receive owned values only; queue game work to LevelTick.
class NativeClientCommands {
public:
    using Handler=std::function<bool(const NativeCommandInvocation&)>;
    enum class Result { unavailable,waiting,unchanged,registered,collision,failed };
    NativeClientCommands()=default;
    NativeClientCommands(const NativeClientCommands&)=delete;
    NativeClientCommands& operator=(const NativeClientCommands&)=delete;
    ~NativeClientCommands(){for(auto& entry:entries_)command_native::routes[entry.slot].store(nullptr);}
    bool add(std::string name,std::string description,CommandIntegerArgument parameter,Handler handler){
        if(ready_||!command_native::nameValid(name)||!command_native::nameValid(parameter.name)||parameter.name.size()>15||!handler||(description.size()>1024||description.find('\0')!=description.npos))return false;
        for(const auto& entry:entries_)if(entry.route->name==name)return false;
        const auto slot=command_native::nextSlot.fetch_add(1);if(slot>=command_native::maxCommands)return false;
        auto route=std::make_shared<command_native::Route>();route->name=std::move(name);route->description=std::move(description);
        route->parameter=std::move(parameter);route->handler=std::move(handler);
        entries_.push_back({slot,route});command_native::routes[slot].store(std::move(route));return true;
    }
    bool add(CommandDefinition definition){
        const auto name=definition.name().value(),description=definition.description().value(),argument=definition.argument().value();
        const bool optional=definition.optional();
        return add(name,description,{argument,optional},[value=std::move(definition)](const NativeCommandInvocation& invocation){
            return value.invoke(invocation.integer)==CommandDecision::Accepted;
        });
    }
    bool load(const RuntimeProfile& profile,std::string& error){
        ready_=false;reset();
        if(!profile.supports({Feature::ClientCommands},error))return false;
        return initialize(profile.json(),error);
    }
    bool load(const std::string& profile,CandidatePolicy policy,std::string& error){
        return load(profile,policy==CandidatePolicy::AllowCandidates,error);
    }
    // Compatibility entry point. New clients use ProfileManager + RuntimeProfile.
    bool load(const std::string& profile,bool allowCandidates,std::string& error){
        ready_=false;reset();
        if(!allowCandidates){error="native command registration requires candidate opt-in";return false;}
        Resolver resolver;
        if(!resolver.loadJson(profile,error)||!resolver.attachWindowsMainModule(error)||!resolver.exactBuild())return false;
        return initialize(profile,error);
    }
private:
    bool initialize(const std::string& profile,std::string& error){
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
        error="native commands require MSVC Release STL ABI";return false;
#endif
        try{
            const auto j=nlohmann::json::parse(profile);const auto& c=j.at("native_client_commands");
            if(c.at("schema")!=1)throw std::runtime_error("unsupported native command schema");
            base_=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));textBegin_=base_+number(j.at("target").at("text_rva"));textEnd_=textBegin_+number(j.at("target").at("text_size"));
            const auto& layout=c.at("layout");
            clientMinecraft_=number(layout.at("client_minecraft_offset"));minecraftCommands_=number(layout.at("minecraft_commands_offset"));commandsRegistry_=number(layout.at("commands_registry_offset"));
            if(clientMinecraft_>65536||minecraftCommands_>65536||commandsRegistry_>65536)throw std::runtime_error("invalid command registry path");
            // This adapter implements this Command/ParameterData/callable ABI.
            // Different shapes require a new adapter, not blind offset changes.
            if(number(layout.at("argument_offset"))!=0x20||number(layout.at("argument_set_offset"))!=0x24||
               number(layout.at("parameter_size"))!=0x78||number(layout.at("overload_size"))!=0x80||
               number(layout.at("signature_overloads_offset"))!=0x40||number(layout.at("output_success_offset"))!=0x28)
                throw std::runtime_error("unsupported native command object ABI");
            command_native::Command probe(0);
            if(reinterpret_cast<const std::byte*>(&probe.argument)-reinterpret_cast<const std::byte*>(&probe)!=0x20||
               reinterpret_cast<const std::byte*>(&probe.hasArgument)-reinterpret_cast<const std::byte*>(&probe)!=0x24)throw std::runtime_error("compiler command ABI mismatch");
            registerFn_=checked(c.at("register_command"));findFn_=checked(c.at("find_command"));parameterFn_=checked(c.at("make_parameter"));overloadFn_=checked(c.at("register_optional_parameter"));
            factoryTable_=reinterpret_cast<void*>(base_+number(c.at("factory_vtable_rva")));
            const auto copy=checked(c.at("factory_copy")),destroy=checked(c.at("factory_destroy")),invoke=checked(c.at("factory_invoke"));
            std::array<void*,4> actual{};
            if(!command_native::read(factoryTable_,0,actual)||actual!=std::array<void*,4>{copy,destroy,invoke,copy})throw std::runtime_error("native command factory mismatch");
            integerType_=reinterpret_cast<void*>(base_+number(c.at("integer_type_rva")));integerRule_=reinterpret_cast<void*>(base_+number(c.at("integer_rule_rva")));
            checked(c.at("client_commands_anchor"));checked(c.at("has_commands_anchor"));
            HMODULE pinned{};
            if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(command_native::factoryFunctions[0]),&pinned))throw std::runtime_error("command provider pin failed");
            ready_=true;error.clear();return true;
        }catch(const std::exception& e){error=e.what();return false;}
    }
public:
    void reset()noexcept{for(auto& entry:entries_)entry.route->registry.store(nullptr);}
    const std::string& error()const noexcept{return error_;}
    // Low-rate owning-client-tick poll: resolves/re-registers after registry
    // replacement. Unrelated commands with the same name are never overwritten.
    Result update(void* client){
        if(!ready_)return Result::unavailable;
        DWORD expected=0;const auto thread=GetCurrentThreadId();ownerThread_.compare_exchange_strong(expected,thread);
        if(ownerThread_.load()!=thread){error_="native command update called outside owning client tick thread";return Result::unavailable;}
        void* minecraft{};void* commands{};void* registry{};void* parser{};std::uint16_t type{};
        if(!command_native::read(client,clientMinecraft_,minecraft)||!command_native::read(minecraft,minecraftCommands_,commands)||
           !command_native::read(commands,commandsRegistry_,registry)||!registry||!command_native::read(integerType_,0,type)||type>4096||
           !command_native::read(integerRule_,0,parser)||!executable(parser)){reset();return Result::waiting;}
        Result result=Result::unchanged;
        for(auto& entry:entries_){
            auto& route=*entry.route;
            void* signature{};
            if(!find(findFn_,registry,route.name,&signature)){
                route.registry.store(nullptr);route.failedRegistry=registry;error_="native command lookup failed: "+route.name;result=Result::failed;continue;
            }
            if(signature){
                if(ours(signature,entry.slot)){route.registry.store(registry);continue;}
                route.registry.store(nullptr);error_="command name already exists: "+route.name;result=Result::collision;continue;
            }
            route.registry.store(nullptr);
            if(route.failedRegistry==registry){result=Result::failed;continue;}
            if(!registerOne(registry,route,entry.slot)||!find(findFn_,registry,route.name,&signature)||!ours(signature,entry.slot)){
                route.failedRegistry=registry;error_="native command registration failed: "+route.name;result=Result::failed;continue;
            }
            route.registry.store(registry);
            if(result==Result::unchanged)result=Result::registered;
        }
        return result;
    }
private:
    struct Entry {std::size_t slot;std::shared_ptr<command_native::Route> route;};
    std::vector<Entry> entries_;
    bool ready_{};std::string error_;std::atomic<DWORD> ownerThread_{0};
    std::uintptr_t base_{},textBegin_{},textEnd_{};
    std::size_t clientMinecraft_{},minecraftCommands_{},commandsRegistry_{};
    void *registerFn_{},*findFn_{},*parameterFn_{},*overloadFn_{},*factoryTable_{},*integerType_{},*integerRule_{};
    static std::uint32_t number(const nlohmann::json& j){const auto n=j.is_string()?std::stoull(j.get<std::string>(),nullptr,0):j.get<std::uint64_t>();if(n>UINT32_MAX)throw std::runtime_error("command profile overflow");return std::uint32_t(n);}
    bool executable(void* p)const noexcept{const auto v=reinterpret_cast<std::uintptr_t>(p);return v>=textBegin_&&v<textEnd_;}
    void* checked(const nlohmann::json& f)const{
        auto* p=reinterpret_cast<void*>(base_+number(f.at("rva")));if(!executable(p))throw std::runtime_error("command target outside .text");
        const auto guard=f.at("entry_check").get<std::string>();if(guard.size()<23||guard.size()>191||(guard.size()+1)%3)throw std::runtime_error("invalid command guard");
        for(std::size_t i=0;i<guard.size();i+=3){unsigned char value{};if(!command_native::read(p,i/3,value)||value!=std::stoul(guard.substr(i,2),nullptr,16))throw std::runtime_error("native command entry mismatch");}return p;
    }
    static bool find(void* fn,void* registry,const std::string& name,void** out)noexcept{
        __try{*out=reinterpret_cast<void*(__fastcall*)(void*,const std::string*)>(fn)(registry,&name);return true;}__except(EXCEPTION_EXECUTE_HANDLER){*out=nullptr;return false;}
    }
    bool ours(void* signature,std::size_t slot)const noexcept{
        void* first{};void* last{};void* table{};void* fn{};
        return command_native::read(signature,0x40,first)&&command_native::read(signature,0x48,last)&&first&&last&&
            reinterpret_cast<std::uintptr_t>(last)>=reinterpret_cast<std::uintptr_t>(first)+0x80&&
            command_native::read(first,8,table)&&table==factoryTable_&&command_native::read(first,16,fn)&&fn==reinterpret_cast<void*>(command_native::factoryFunctions[slot]);
    }
    bool registerOne(void* registry,const command_native::Route& route,std::size_t slot)const noexcept{
        alignas(8) std::array<std::byte,0x78> parameter{};
        alignas(8) std::array<std::uintptr_t,8> factory{};
        factory[0]=reinterpret_cast<std::uintptr_t>(factoryTable_);factory[1]=reinterpret_cast<std::uintptr_t>(command_native::factoryFunctions[slot]);
        __try{
            reinterpret_cast<void*(__fastcall*)(void*,const void*,const void*,const char*,int,const char*,const char*,int,bool,int)>(parameterFn_)(
                parameter.data(),integerType_,integerRule_,route.parameter.name.c_str(),0,nullptr,nullptr,0x20,route.parameter.optional,0x24);
            std::size_t size{},capacity{};
            if(!command_native::read(parameter.data(),0x28,size)||!command_native::read(parameter.data(),0x30,capacity)||size!=route.parameter.name.size()||capacity!=15)return false;
            reinterpret_cast<void(__fastcall*)(void*,const std::string*,const char*,unsigned char,unsigned short,unsigned short)>(registerFn_)(
                registry,&route.name,route.description.c_str(),0,0x10,0x80); // native Help: Any / SyncLocal / NotCheat
            return reinterpret_cast<void*(__fastcall*)(void*,const char*,std::uint64_t,void*,const void*)>(overloadFn_)(
                registry,route.name.c_str(),std::uint64_t{0x7fffffff00000001},factory.data(),parameter.data())!=nullptr;
        }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
};
} // namespace bedrock
