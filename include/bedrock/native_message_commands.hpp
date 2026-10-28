#pragma once
#include <bedrock/profile_json.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace bedrock {
// Register server-bound message command metadata in the real CommandRegistry.
// The game's /me parser and native callable are copied by native constructors;
// outgoing requests are handled separately by NativeCommandRequests. No SDK
// vtable, allocator, or callback is stored in the game's registry.
class NativeMessageCommands {
public:
    enum class State { WaitingClient, WaitingRegistry, WaitingTemplate, Registered, Collision, Failed };
    bool add(std::string name, std::string description) {
        if (loaded_ || name.empty() || name.size()>64 || description.size()>1024 ||
            description.find('\0')!=std::string::npos ||
            name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-")!=std::string::npos || entries_.size()>=32) return false;
        for (const auto& entry:entries_) if (entry.name==name) return false;
        entries_.push_back({std::move(name),std::move(description)}); return true;
    }
    bool load(const RuntimeProfile& profile, std::string& error) {
        loaded_=false;
        try {
            if (!profile.resolver().exactBuild() || profile.candidatePolicy()!=CandidatePolicy::AllowCandidates)
                throw std::runtime_error("message command registration requires an exact build and AllowCandidates");
            const auto root=parseNativeProfile(profile.json());
            const auto& layout=root.at("native_client_commands").at("message_forwarding");
            if (layout.value("revision",0)!=1 || layout.at("module_sha256")!=root.at("target").at("sha256"))
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
            factory_=native_profile::checked(root,layout.at("message_factory"));
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
    // Call on the owning client tick. Lookup is read-only until /me's complete
    // one-message overload has been validated. Existing names are preserved.
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
        void* signature{};
        const std::string source="me";
        if (!lookup(find_,registry,&source,&signature)) { error_="CommandRegistry::findCommand(me) failed"; return State::Failed; }
        if (!signature) return State::WaitingTemplate;
        std::array<std::byte,64> factory;
        std::array<std::byte,0x78> parameter;
        if (!messageOverload(signature,factory,parameter)) {
            error_="/me message overload differs from the exact-build native layout"; return State::Failed;
        }
        for (auto& entry:entries_) {
            void* existing{};
            if (!lookup(find_,registry,&entry.name,&existing)) { error_="CommandRegistry lookup failed: "+entry.name; return State::Failed; }
            if (existing) {
                if (entry.registry==registry && entry.signature==existing && ownsOverload(existing)) continue;
                error_="command name already exists; preserved: /"+entry.name; return State::Collision;
            }
            if (entry.failedRegistry==registry) { error_="previous registration failed: /"+entry.name; return State::Failed; }
            // The overload helper consumes its callable argument. Borrowed /me
            // metadata is never passed as mutable storage; use a fresh copy.
            auto callable=factory;
            void* created{};
            if (!registerName(register_,registry,&entry.name,entry.description.c_str()) ||
                !addOverload(overload_,registry,entry.name.c_str(),callable.data(),parameter.data()) ||
                !lookup(find_,registry,&entry.name,&created) || !created || !ownsOverload(created)) {
                entry.failedRegistry=registry; error_="native message registration failed: /"+entry.name; return State::Failed;
            }
            entry.registry=registry; entry.signature=created;
        }
        error_.clear(); return State::Registered;
    }
    const std::string& error() const noexcept { return error_; }
private:
    struct Entry { std::string name,description; void* registry{}; void* signature{}; void* failedRegistry{}; };
    bool messageOverload(void* signature, std::array<std::byte,64>& callable,
                         std::array<std::byte,0x78>& parameter) const noexcept {
        std::uintptr_t begin{},end{},capacity{},first{},last{};
        if (!native_profile::read(signature,0x40,begin) || !native_profile::read(signature,0x48,end) ||
            !native_profile::read(signature,0x50,capacity) || !begin || end<begin || capacity<end ||
            (end-begin)%0x80 || (end-begin)/0x80<1 || (capacity-begin)>0x100000) return false;
        const auto* overload=reinterpret_cast<void*>(begin);
        void* table{}; void* factory{};
        if (!native_profile::read(overload,8,table) || table!=factoryTable_ ||
            !native_profile::read(overload,16,factory) || factory!=factory_ ||
            !native_profile::read(overload,0x48,first) || !first || !native_profile::read(overload,0x50,last) ||
            last<first || last-first!=0x78 || !native_profile::read(overload,8,callable) ||
            !native_profile::read(reinterpret_cast<void*>(first),0,parameter)) return false;
        std::size_t length{},capacityName{};
        std::memcpy(&length,parameter.data()+0x28,8); std::memcpy(&capacityName,parameter.data()+0x30,8);
        return length==7 && capacityName==15 && std::memcmp(parameter.data()+0x18,"message",7)==0;
    }
    bool ownsOverload(void* signature) const noexcept {
        std::array<std::byte,64> factory;
        std::array<std::byte,0x78> parameter;
        return messageOverload(signature,factory,parameter);
    }
    static bool lookup(void* fn,void* registry,const std::string* name,void** output) noexcept {
        __try { *output=reinterpret_cast<void*(__fastcall*)(void*,const std::string*)>(fn)(registry,name); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { *output=nullptr; return false; }
    }
    static bool registerName(void* fn,void* registry,const std::string* name,const char* description) noexcept {
        // Exactly the /me routing flags: server-bound (0x40), NotCheat (0x80).
        // SyncLocal (0x10) would execute the native /me factory locally.
        __try { reinterpret_cast<void(__fastcall*)(void*,const std::string*,const char*,unsigned char,unsigned short,unsigned short)>(fn)(registry,name,description,0,0x40,0x80); return true; }
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
    void *register_{},*find_{},*overload_{},*factoryTable_{},*factory_{};
    bool loaded_{};
    std::string error_;
};
} // namespace bedrock
