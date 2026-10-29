#pragma once
#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/commands/command_requests.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <new>

namespace bedrock {
// Exact-build local Command ABI. The native parser owns CommandMessage's
// allocations; its native destructor releases them. Route callbacks copy text
// into an SDK queue and never send packets from the parser/execute callback.
struct NativeMessageInvocation {
    std::string line;
    std::int32_t version{};
    CommandOriginData origin;
    bool contextAvailable{};
};
namespace message_command_native {
struct Route {
    std::string name;
    std::function<void(NativeMessageInvocation)> handler;
    std::atomic<const void*> registry{};
    void *resolve{},*destroy{},*free{};
    std::uintptr_t textBegin{},textEnd{};
};
inline std::array<std::shared_ptr<Route>,32> routes;
inline std::size_t nextSlot{}; // definitions are added before hooks start
inline void destroyMessage(void* fn,void* value) noexcept {
    __try { reinterpret_cast<void(__fastcall*)(void*)>(fn)(value); }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
}
inline bool resolveMessage(void* fn,const void* message,void* result,const void* origin) noexcept {
    __try { reinterpret_cast<void*(__fastcall*)(const void*,void*,const void*,int)>(fn)(message,result,origin,4096); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline void releaseString(void* fn,void* buffer) noexcept {
    // The result uses Minecraft's allocator, not the DLL's CRT heap.
    __try {
        auto* words=static_cast<std::uintptr_t*>(buffer);
        if (words[3]<16) return;
        auto* allocation=reinterpret_cast<void*>(words[0]); auto size=words[3]+1;
        if (size>=0x1000) {
            const auto raw=*(reinterpret_cast<std::uintptr_t*>(allocation)-1);
            if (words[0]-raw-8>0x1f) return;
            allocation=reinterpret_cast<void*>(raw); size+=0x27;
        }
        reinterpret_cast<void(__fastcall*)(void*,std::size_t)>(fn)(allocation,size);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
inline bool captureOrigin(const Route& route,const void* origin,void* result) noexcept {
    void* table{}; void* fn{};
    if (!native_profile::read(origin,0,table) || !native_profile::read(table,0xd0,fn)) return false;
    const auto address=reinterpret_cast<std::uintptr_t>(fn);
    if (address<route.textBegin || address>=route.textEnd) return false;
    // CommandRequestData constructor at RVA0x419CBC0 uses this exact virtual call.
    __try { return reinterpret_cast<void*(__fastcall*)(const void*,void*)>(fn)(origin,result)==result; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
class Command {
public:
    explicit Command(std::size_t index):routeIndex(index) {}
    virtual ~Command() { if (const auto& route=routes[routeIndex]; route && route->destroy) destroyMessage(route->destroy,message.data()); }
    virtual bool collectOptionalArguments() { return true; }
    virtual void execute(const void* origin,void*) const {
        const auto route=routes[routeIndex];
        if (!route || !registry || route->registry.load()!=registry) return;
        alignas(8) std::array<std::byte,0x28> result{};
        const bool resolved=resolveMessage(route->resolve,message.data(),result.data(),origin);
        try {
            if (resolved && result[0x20]==std::byte{1}) {
                std::size_t length{},capacity{}; const char* text{};
                std::memcpy(&length,result.data()+0x10,8); std::memcpy(&capacity,result.data()+0x18,8);
                if (capacity<16) text=reinterpret_cast<const char*>(result.data());
                else std::memcpy(&text,result.data(),8);
                if (length<=4096 && text) {
                    NativeMessageInvocation invocation;
                    invocation.line="/"+route->name+" "+std::string(text,length);
                    invocation.version=version;
                    alignas(8) std::array<std::byte,0x40> data{};
                    if (captureOrigin(*route,origin,data.data())) {
                        std::memcpy(&invocation.origin.type,data.data(),1);
                        std::memcpy(invocation.origin.uuid.data(),data.data()+8,16);
                        std::memcpy(&invocation.origin.playerId,data.data()+0x38,8);
                        invocation.contextAvailable=true;
                        releaseString(route->free,data.data()+0x18);
                    }
                    route->handler(std::move(invocation));
                }
            }
        } catch (...) {} // no C++ exceptions across native virtual calls
        releaseString(route->free,result.data());
    }
    int version{}; std::uint32_t padding{};
    const void* registry{};
    int symbol{-1}; std::uint8_t permission{5},reserved{}; std::uint16_t flags{};
    std::uint8_t messageFlags{1}; std::array<std::byte,7> padding2{};
    std::array<std::byte,0x20> message{}; // MeCommand factory/destructor: +0x28
    std::size_t routeIndex{};
};
static_assert(sizeof(Command)==0x50);
using Factory=void*(__fastcall*)(void*);
template<std::size_t I> void* __fastcall create(void* result) {
    auto* command=new(std::nothrow) Command(I); std::memcpy(result,&command,8); return result;
}
template<std::size_t... I> constexpr auto factories(std::index_sequence<I...>) {
    return std::array<Factory,sizeof...(I)>{&create<I>...};
}
inline constexpr auto factoryFunctions=factories(std::make_index_sequence<32>{});
}
// Native registration follows Top's SyncLocal command/factory path. It does
// not depend on the server's /me signature, proxy factory, or overload order.
class NativeMessageCommands {
public:
    enum class State { WaitingClient, WaitingRegistry, WaitingTemplate, Registered, Collision, Failed };
    bool add(std::string name, std::string description, std::function<void(NativeMessageInvocation)> handler) {
        if (loaded_ || !handler || name.empty() || name.size()>64 || description.size()>1024 ||
            description.find('\0')!=std::string::npos ||
            name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-")!=std::string::npos || entries_.size()>=32) return false;
        for (const auto& entry:entries_) if (entry.name==name) return false;
        if (message_command_native::nextSlot>=32) return false;
        const auto slot=message_command_native::nextSlot++;
        auto route=std::make_shared<message_command_native::Route>();
        route->name=name; route->handler=std::move(handler);
        message_command_native::routes[slot]=route;
        entries_.push_back({std::move(name),std::move(description),slot,std::move(route)}); return true;
    }
    bool load(const RuntimeProfile& profile, std::string& error) {
        loaded_=false;
        try {
            if (!profile.resolver().exactBuild() || profile.candidatePolicy()!=CandidatePolicy::AllowCandidates)
                throw std::runtime_error("message command registration requires an exact build and AllowCandidates");
            const auto root=parseNativeProfile(profile.json());
            const auto& layout=root.at("native_client_commands").at("message_forwarding");
            if (layout.value("revision",0)!=3 || layout.at("module_sha256")!=root.at("target").at("sha256"))
                throw std::runtime_error("unsupported CommandRegistry message layout or executable identity");
            base_=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            clientMinecraft_=native_profile::number(layout.at("client_minecraft_offset"));
            minecraftCommands_=native_profile::number(layout.at("minecraft_commands_offset"));
            commandsRegistry_=native_profile::number(layout.at("commands_registry_offset"));
            if (clientMinecraft_!=0x1d0 || minecraftCommands_!=0xc8 || commandsRegistry_!=0x10)
                throw std::runtime_error("unsupported CommandRegistry object path revision");
            register_=native_profile::checked(root,layout.at("register_command"));
            find_=native_profile::checked(root,layout.at("find_command"));
            overload_=native_profile::checked(root,layout.at("register_overload"));
            native_profile::checked(root,layout.at("client_commands_anchor"));
            native_profile::checked(root,layout.at("native_request_data_constructor"));
            native_profile::checked(root,layout.at("command_factory_context"));
            resolve_=native_profile::checked(root,layout.at("resolve_message"));
            destroy_=native_profile::checked(root,layout.at("destroy_message"));
            free_=native_profile::checked(root,layout.at("free_memory"));
            type_=reinterpret_cast<void*>(base_+native_profile::number(layout.at("message_type_rva")));
            rule_=reinterpret_cast<void*>(base_+native_profile::number(layout.at("message_rule_rva")));
            message_command_native::Command probe(0);
            if (reinterpret_cast<const std::byte*>(&probe.message)-reinterpret_cast<const std::byte*>(&probe)!=0x28)
                throw std::runtime_error("compiler CommandMessage offset mismatch");
            HMODULE pinned{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(message_command_native::factoryFunctions[0]),&pinned))
                throw std::runtime_error("command provider pin failed");
            for (auto& entry:entries_) {
                entry.route->resolve=resolve_; entry.route->destroy=destroy_; entry.route->free=free_;
                entry.route->textBegin=base_+native_profile::number(root.at("target").at("text_rva"));
                entry.route->textEnd=entry.route->textBegin+native_profile::number(root.at("target").at("text_size"));
            }
            factoryTable_=reinterpret_cast<void*>(base_+native_profile::number(layout.at("factory_vtable_rva")));
            const auto& functions=layout.at("factory_functions");
            if (!functions.is_array() || functions.size()!=4) throw std::runtime_error("invalid message callable definition");
            for (std::size_t i=0;i<4;++i) {
                void* actual{};
                if (!native_profile::read(factoryTable_,i*8,actual) || actual!=native_profile::checked(root,functions.at(i)))
                    throw std::runtime_error("native message callable vtable mismatch");
            }
            loaded_=true; error.clear(); return true;
        } catch (const std::exception& e) { error=e.what(); return false; }
    }
    // Call on the owning tick. Existing command names are preserved.
    State update(void* client) {
        if (!loaded_) { error_="message command adapter is not loaded"; return State::Failed; }
        if (!client) return State::WaitingClient;
        DWORD expected{}; const auto thread=GetCurrentThreadId();
        owner_.compare_exchange_strong(expected,thread);
        if (owner_!=thread) { error_="CommandRegistry update is outside the owning tick thread"; return State::Failed; }
        void* minecraft{}; void* commands{}; void* registry{};
        if (!native_profile::read(client,clientMinecraft_,minecraft) || !minecraft ||
            !native_profile::read(minecraft,minecraftCommands_,commands) || !commands ||
            !native_profile::read(commands,commandsRegistry_,registry) || !registry) return State::WaitingRegistry;
        std::uint16_t type{}; void* parser{};
        if (!native_profile::read(type_,0,type) || !type || type>4096 || !native_profile::read(rule_,0,parser) || !parser)
            return State::WaitingTemplate;
        alignas(8) std::array<std::byte,0x78> parameter{};
        const auto put=[&](std::size_t offset,const auto& value) { std::memcpy(parameter.data()+offset,&value,sizeof(value)); };
        put(0,type); put(8,rule_);
        std::memcpy(parameter.data()+0x18,"message",7);
        put(0x28,std::uint64_t{7}); put(0x30,std::uint64_t{15});
        put(0x40,std::int32_t{-1}); put(0x50,std::int32_t{-1});
        put(0x58,std::int32_t{0x28}); put(0x5c,std::int32_t{-1});
        for (auto& entry:entries_) {
            void* existing{};
            if (!lookup(find_,registry,&entry.name,&existing)) { error_="CommandRegistry lookup failed: "+entry.name; return State::Failed; }
            if (existing) {
                if (entry.registry==registry && entry.signature==existing && ownsOverload(existing,entry.slot)) continue;
                error_="command name already exists; preserved: /"+entry.name; return State::Collision;
            }
            if (entry.failedRegistry==registry) { error_="previous registration failed: /"+entry.name; return State::Failed; }
            // The native overload helper consumes a fresh callable.
            alignas(8) std::array<std::byte,64> callable{};
            auto factory=message_command_native::factoryFunctions[entry.slot];
            std::memcpy(callable.data(),&factoryTable_,8); std::memcpy(callable.data()+8,&factory,8);
            void* created{};
            if (!registerName(register_,registry,&entry.name,entry.description.c_str()) ||
                !addOverload(overload_,registry,entry.name.c_str(),callable.data(),parameter.data()) ||
                !lookup(find_,registry,&entry.name,&created) || !created || !ownsOverload(created,entry.slot)) {
                entry.failedRegistry=registry; error_="native message registration failed: /"+entry.name; return State::Failed;
            }
            entry.registry=registry; entry.signature=created; entry.route->registry=registry;
        }
        error_.clear(); return State::Registered;
    }
    const std::string& error() const noexcept { return error_; }
private:
    struct Entry {
        std::string name,description; std::size_t slot{}; std::shared_ptr<message_command_native::Route> route;
        void* registry{}; void* signature{}; void* failedRegistry{};
    };
    bool ownsOverload(void* signature,std::size_t slot) const noexcept {
        std::uintptr_t begin{},end{}; void* table{}; message_command_native::Factory factory{};
        return native_profile::read(signature,0x40,begin) && begin &&
            native_profile::read(signature,0x48,end) && end-begin==0x80 &&
            native_profile::read(reinterpret_cast<void*>(begin),8,table) && table==factoryTable_ &&
            native_profile::read(reinterpret_cast<void*>(begin),16,factory) && factory==message_command_native::factoryFunctions[slot];
    }
    static bool lookup(void* fn,void* registry,const std::string* name,void** output) noexcept {
        __try { *output=reinterpret_cast<void*(__fastcall*)(void*,const std::string*)>(fn)(registry,name); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { *output=nullptr; return false; }
    }
    static bool registerName(void* fn,void* registry,const std::string* name,const char* description) noexcept {
        // Same local routing as Top: SyncLocal (0x10), NotCheat (0x80).
        __try { reinterpret_cast<void(__fastcall*)(void*,const std::string*,const char*,unsigned char,unsigned short,unsigned short)>(fn)(registry,name,description,0,0x10,0x80); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool addOverload(void* fn,void* registry,const char* name,void* factory,const void* parameter) noexcept {
        __try { return reinterpret_cast<void*(__fastcall*)(void*,const char*,std::uint64_t,void*,const void*)>(fn)(registry,name,0x7fffffff00000001ull,factory,parameter)!=nullptr; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    std::vector<Entry> entries_;
    std::atomic<DWORD> owner_{};
    std::uintptr_t base_{};
    std::uint32_t clientMinecraft_{},minecraftCommands_{},commandsRegistry_{};
    void *register_{},*find_{},*overload_{},*factoryTable_{},*resolve_{},*destroy_{},*free_{},*type_{},*rule_{};
    bool loaded_{};
    std::string error_;
};
} // namespace bedrock
