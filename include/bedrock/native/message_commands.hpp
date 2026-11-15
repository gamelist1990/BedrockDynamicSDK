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
    bool enumFlavor{};
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
        if (route->enumFlavor) {
            // Enum-parsed argument storage is not a CommandMessage: report
            // what the parser left in the argument slot so the handler can
            // log it. Only an SSO string (inline bytes, capacity<16) may be
            // read as text — never follow a heap pointer out of the slot: a
            // parsed enum result can masquerade as a string header with a
            // wild data pointer, and dereferencing it crashes the game
            // (observed 2026-11-16). Everything else is reported as raw hex.
            try {
                const auto* slot=reinterpret_cast<const unsigned char*>(message.data());
                static constexpr char digits[]="0123456789abcdef";
                std::string hex; hex.reserve(64);
                for (int i=0;i<0x20;++i) { hex+=digits[slot[i]>>4]; hex+=digits[slot[i]&15]; }
                std::size_t length{},capacity{};
                std::memcpy(&length,slot+0x10,8); std::memcpy(&capacity,slot+0x18,8);
                bool sso = capacity<16 && length<=capacity;
                if (sso) for (std::size_t i=0;i<length;++i)
                    if (slot[i]<0x20 || slot[i]>=0x7f) { sso=false; break; }
                NativeMessageInvocation invocation;
                if (sso && length>0) {
                    invocation.line="/"+route->name+" "+std::string(reinterpret_cast<const char*>(slot),length)+" [raw "+hex+"]";
                } else {
                    invocation.line="/"+route->name+" __raw "+hex;
                }
                invocation.version=version;
                route->handler(std::move(invocation));
            } catch (...) {}
            return;
        }
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
    // Optional raw parameter override: build the CommandParameterData the
    // way a vanilla command setup does for a non-message type (e.g. an enum
    // parameter), given the descriptor object RVA, the type's symbol slot
    // RVA, the global symbol counter RVA, the +0x58 type id and the +0x60
    // flags word. The symbol id is allocated from the shared counter on
    // first use, exactly like the vanilla lazy allocation.
    struct RawParameter {
        std::uint32_t descriptorRva{};
        std::uint32_t symbolSlotRva{};
        std::uint32_t symbolCounterRva{};
        std::int32_t typeId{};
        std::uint32_t flags60{};
        // Written into the descriptor object (with the message rule as a
        // stand-in parser) when the client left it zero-initialised, so
        // the type resolves in autocomplete: the descriptor flags word is
        // the parameter's wire type (e.g. 0x300018 = hard enum #24).
        std::uint32_t descriptorFlags{};
    };
    // EXPERIMENT (packet-style enum parameter): register a hard enum into
    // the registry's own enum store with the native registration function
    // the AvailableCommands conversion uses (RVA 0x55A8430: appends a
    // {name, values} entry to the vector at registry+0x230 and indexes it
    // in the map at registry+0x248), then build the parameter exactly the
    // way the conversion builds packet-command parameters: shared symbol,
    // no descriptor, the category rule function at +0x10 (hard enum:
    // RVA 0x557B040), type word (enum index | 0x300000) at +0x40,
    // 0xFFFFFFFF at +0x50, kind 1 at +0x54, -1 at +0x58, options byte
    // at +0x61.
    struct EnumParameter {
        std::uint32_t registerEnumRva{};
        std::uint32_t enumRuleRva{};
        std::uint32_t sharedSymbolSlotRva{};
        std::uint32_t symbolCounterRva{};
        std::string enumName;
        std::vector<std::string> values;
        std::uint8_t optionsByte{2};
        // Experiment: instead of registering a new enum, reference an
        // existing enum index in the packet-converted store (e.g. the
        // Block enum) directly.
        std::uint32_t presetIndex{};
        bool hasPresetIndex{};
    };
    // parameterName is only the label autocomplete shows for the message
    // argument (defaults to "message", as in /me); the parser type is the
    // CommandMessage type regardless of the label.
    bool add(std::string name, std::string description, std::function<void(NativeMessageInvocation)> handler,
             std::string parameterName = "message", bool parameterOptional = false,
             const RawParameter* rawParameter = nullptr,
             const EnumParameter* enumParameter = nullptr) {
        if (loaded_ || !handler || name.empty() || name.size()>64 || description.size()>1024 ||
            description.find('\0')!=std::string::npos ||
            name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-")!=std::string::npos || entries_.size()>=32) return false;
        if (parameterName.empty() || parameterName.size()>15 ||
            parameterName.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")!=std::string::npos) return false;
        for (const auto& entry:entries_) if (entry.name==name) return false;
        if (message_command_native::nextSlot>=32) return false;
        const auto slot=message_command_native::nextSlot++;
        auto route=std::make_shared<message_command_native::Route>();
        route->name=name; route->handler=std::move(handler);
        if (enumParameter) route->enumFlavor=true;
        message_command_native::routes[slot]=route;
        Entry entry{std::move(name),std::move(description),slot,std::move(route),std::move(parameterName),parameterOptional};
        if (rawParameter) { entry.hasRaw=true; entry.raw=*rawParameter; }
        if (enumParameter) { entry.hasEnum=true; entry.enumSpec=*enumParameter; }
        entries_.push_back(std::move(entry)); return true;
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
        for (auto& entry:entries_) {
            // CommandParameterData is built per entry so each command can
            // label its message argument (the name lives in the parameter).
            alignas(8) std::array<std::byte,0x78> parameter{};
            const auto put=[&](std::size_t offset,const auto& value) { std::memcpy(parameter.data()+offset,&value,sizeof(value)); };
            if (entry.hasEnum) {
                // Register the enum into this registry once (the native
                // registration moves the values vector into the registry's
                // enum store and returns index | 0x4100000 for a new enum),
                // allocate the shared conversion symbol if needed, then
                // build the parameter in the converted packet layout.
                if (entry.enumSpec.hasPresetIndex) {
                    entry.enumIndex=entry.enumSpec.presetIndex;
                    entry.enumRegistry=registry;
                    enumDiagnostic="enumpreset: idx="+std::to_string(entry.enumIndex);
                } else if (entry.enumRegistry!=registry) {
                    struct EnumData { std::string name; std::vector<std::string> values; };
                    EnumData data; data.name=entry.enumSpec.enumName;
                    std::vector<std::string> values=entry.enumSpec.values;
                    std::uint64_t result{};
                    if (!registerEnum(reinterpret_cast<void*>(base_+entry.enumSpec.registerEnumRva),
                                      registry,&data,&values,result)) {
                        error_="hard enum registration faulted: "+entry.name; return State::Failed;
                    }
                    entry.enumIndex=static_cast<std::uint32_t>(result)&0xe00fffffu;
                    entry.enumRegistry=registry;
                    { char buf[160]; std::snprintf(buf,sizeof(buf),
                        "enumreg: raw=0x%llx idx=%u name='%s' values=%zu",
                        static_cast<unsigned long long>(result),entry.enumIndex,
                        entry.enumSpec.enumName.c_str(),entry.enumSpec.values.size());
                      enumDiagnostic=buf; }
                }
                std::uint16_t sharedSymbol{};
                __try {
                    auto* slot=reinterpret_cast<std::uint16_t*>(base_+entry.enumSpec.sharedSymbolSlotRva);
                    auto* counter=reinterpret_cast<std::uint16_t*>(base_+entry.enumSpec.symbolCounterRva);
                    if (*slot==0) { *slot=*counter; *counter=static_cast<std::uint16_t>(*counter+1); }
                    sharedSymbol=*slot;
                } __except(EXCEPTION_EXECUTE_HANDLER) { error_="enum symbol allocation faulted: "+entry.name; return State::Failed; }
                put(0,sharedSymbol);
                put(8,static_cast<void*>(nullptr));
                put(0x10,reinterpret_cast<void*>(base_+entry.enumSpec.enumRuleRva));
                std::memcpy(parameter.data()+0x18,entry.parameterName.data(),entry.parameterName.size());
                put(0x28,static_cast<std::uint64_t>(entry.parameterName.size())); put(0x30,std::uint64_t{15});
                put(0x40,static_cast<std::uint32_t>(entry.enumIndex|0x300000u));
                put(0x50,std::uint32_t{0xffffffffu}); put(0x54,std::uint32_t{1});
                put(0x58,std::int32_t{-1});
                if (entry.parameterOptional) parameter[0x60]=std::byte{1};
                parameter[0x61]=static_cast<std::byte>(entry.enumSpec.optionsByte);
            } else if (entry.hasRaw) {
                // Vanilla-style parameter for a non-message type: allocate
                // the type symbol from the shared counter on first use
                // (the vanilla lazy allocation writes the id into the
                // type's slot and bumps the counter), then reference the
                // type's own descriptor object.
                if (!entry.rawAllocated) {
                    auto* slot=reinterpret_cast<std::uint16_t*>(base_+entry.raw.symbolSlotRva);
                    auto* counter=reinterpret_cast<std::uint16_t*>(base_+entry.raw.symbolCounterRva);
                    __try {
                        if (*slot==0) { *slot=*counter; *counter=static_cast<std::uint16_t>(*counter+1); }
                        entry.rawSymbol=*slot; entry.rawAllocated=true;
                        // If the client never initialised this type's
                        // descriptor (server-only enum types are zero on
                        // the client), fill it so the type resolves:
                        // rule = the message parser (stores the token as a
                        // string, which the invocation path already
                        // extracts), flags = the wire type, symWord = the
                        // per-type handle ((0x80002884 + id) << 32) | id
                        // observed on every built-in descriptor.
                        auto* desc=reinterpret_cast<std::uint64_t*>(base_+entry.raw.descriptorRva);
                        if (desc[0]==0 && parser) {
                            desc[2]=((0x80002884ull+entry.rawSymbol)<<32)|entry.rawSymbol;
                            desc[1]=entry.raw.descriptorFlags;
                            desc[0]=reinterpret_cast<std::uint64_t>(parser);
                        }
                    } __except(EXCEPTION_EXECUTE_HANDLER) { error_="raw parameter symbol allocation faulted: "+entry.name; return State::Failed; }
                }
                put(0,entry.rawSymbol);
                put(8,reinterpret_cast<void*>(base_+entry.raw.descriptorRva));
                std::memcpy(parameter.data()+0x18,entry.parameterName.data(),entry.parameterName.size());
                put(0x28,static_cast<std::uint64_t>(entry.parameterName.size())); put(0x30,std::uint64_t{15});
                put(0x40,std::int32_t{-1}); put(0x50,std::int32_t{-1});
                put(0x58,entry.raw.typeId); put(0x5c,std::int32_t{-1});
                put(0x60,entry.raw.flags60);
            } else {
            put(0,type); put(8,rule_);
            std::memcpy(parameter.data()+0x18,entry.parameterName.data(),entry.parameterName.size());
            put(0x28,static_cast<std::uint64_t>(entry.parameterName.size())); put(0x30,std::uint64_t{15});
            put(0x40,std::int32_t{-1}); put(0x50,std::int32_t{-1});
            put(0x58,std::int32_t{0x28}); put(0x5c,std::int32_t{-1});
            // Same optional flag the integer adapter uses (+0x60): the
            // parser then accepts the command with no token and renders
            // the argument as [name: string] in autocomplete.
            if (entry.parameterOptional) parameter[0x60]=std::byte{1};
            }
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
        for (auto& entry:entries_) {
            if (!entry.hasEnum || entry.enumRegistry!=registry || entry.enumSpec.hasPresetIndex) continue;
            EnumStoreProbe probe{};
            if (probeEnumStore(registry,entry.enumIndex,0,probe)) {
                if (std::strcmp(probe.myName,entry.enumSpec.enumName.c_str())!=0) {
                    // The client rebuilt the enum store under the same
                    // registry object (session join): our index now names a
                    // different enum. Re-register and patch the stored
                    // parameter's type word to the fresh index.
                    struct EnumData { std::string name; std::vector<std::string> values; };
                    EnumData data; data.name=entry.enumSpec.enumName;
                    std::vector<std::string> values=entry.enumSpec.values;
                    std::uint64_t result{};
                    if (registerEnum(reinterpret_cast<void*>(base_+entry.enumSpec.registerEnumRva),
                                     registry,&data,&values,result)) {
                        const auto newIndex=static_cast<std::uint32_t>(result)&0xe00fffffu;
                        const bool patched=entry.signature &&
                            patchStoredEnumParam(entry.signature,entry.parameterName.c_str(),
                                entry.parameterName.size(),
                                reinterpret_cast<void*>(base_+entry.enumSpec.enumRuleRva),
                                newIndex|0x300000u);
                        entry.enumIndex=newIndex;
                        enumDiagnostic="enumfix: store rebuilt (was '"+std::string(probe.myName)+
                            "'); re-registered idx="+std::to_string(newIndex)+
                            (patched?" param patched":" PARAM PATCH FAILED");
                        diagRegistry_=registry; diagCount_=probe.count;
                        diagVals_=static_cast<std::uint32_t>(entry.enumSpec.values.size());
                        diagName_=entry.enumSpec.enumName;
                    }
                } else if (diagRegistry_!=registry || diagCount_!=probe.count ||
                           diagVals_!=probe.myVals || diagName_!=probe.myName) {
                    diagRegistry_=registry; diagCount_=probe.count;
                    diagVals_=probe.myVals; diagName_=probe.myName;
                    char buf[512];
                    std::snprintf(buf,sizeof(buf),
                        "enumdiag: count=%zu myIdx=%u name='%s' vals=%u w38=0x%x w40=0x%x | idx0 name='%s' vals=%u",
                        probe.count,entry.enumIndex,probe.myName,probe.myVals,probe.myW38,probe.myW40,
                        probe.altName,probe.altVals);
                    enumDiagnostic=buf;
                }
            }
            break;
        }
        error_.clear(); return State::Registered;
    }
    const std::string& error() const noexcept { return error_; }
    // TEMP DIAGNOSTIC (E3): human-readable probe of the registry enum
    // store for the first enum entry; updated when the store changes.
    std::string enumDiagnostic;
private:
    struct EnumStoreProbe {
        std::size_t count{};
        std::uint32_t myVals{}; char myName[24]{}; std::uint32_t myW38{}; std::uint32_t myW40{};
        std::uint32_t altVals{}; char altName[24]{};
    };
    static void readEnumEntryRaw(const std::byte* begin, std::uint32_t i, char* nameOut,
                                 std::uint32_t& valsOut, std::uint32_t* w38, std::uint32_t* w40) noexcept {
        const auto* e = begin + static_cast<std::size_t>(i) * 0x38;
        std::uint64_t sz{}; std::memcpy(&sz, e + 0x10, 8);
        const std::byte* data = e;
        if (sz > 15) std::memcpy(&data, e, 8);
        const std::size_t n = sz < 23 ? static_cast<std::size_t>(sz) : 23;
        if (data) std::memcpy(nameOut, data, n);
        nameOut[n] = 0;
        void* vb{}; void* ve{};
        std::memcpy(&vb, e + 0x20, 8); std::memcpy(&ve, e + 0x28, 8);
        valsOut = (vb && ve && ve >= vb)
            ? static_cast<std::uint32_t>((reinterpret_cast<std::uintptr_t>(ve) - reinterpret_cast<std::uintptr_t>(vb)) / 0x20) : 0;
        if (w38) std::memcpy(w38, e + 0x38, 4);
        if (w40) std::memcpy(w40, e + 0x40, 4);
    }
    static bool probeEnumStore(void* registry, std::uint32_t index, std::uint32_t altIndex, EnumStoreProbe& out) noexcept {
        __try {
            const auto* base = static_cast<const std::byte*>(registry);
            void* begin{}; void* end{};
            std::memcpy(&begin, base + 0x230, 8); std::memcpy(&end, base + 0x238, 8);
            if (!begin || end < begin) return false;
            out.count = (reinterpret_cast<std::uintptr_t>(end) - reinterpret_cast<std::uintptr_t>(begin)) / 0x38;
            if (index < out.count) readEnumEntryRaw(static_cast<const std::byte*>(begin), index, out.myName, out.myVals, &out.myW38, &out.myW40);
            else { std::memcpy(out.myName, "<oob>", 6); out.myVals = 0; }
            if (altIndex < out.count) readEnumEntryRaw(static_cast<const std::byte*>(begin), altIndex, out.altName, out.altVals, nullptr, nullptr);
            else { std::memcpy(out.altName, "<oob>", 6); out.altVals = 0; }
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // Rewrite the type word of the first stored parameter of a signature
    // (signature -> first overload record -> params vector at +0x48).
    // Verifies the stored rule pointer and parameter name before writing.
    static bool patchStoredEnumParam(void* signature, const char* paramName, std::size_t paramNameLen,
                                     void* ruleVa, std::uint32_t typeWord) noexcept {
        __try {
            void* ovBegin{}; void* ovEnd{};
            std::memcpy(&ovBegin, static_cast<std::byte*>(signature) + 0x40, 8);
            std::memcpy(&ovEnd, static_cast<std::byte*>(signature) + 0x48, 8);
            if (!ovBegin || ovEnd < ovBegin) return false;
            auto* record = static_cast<std::byte*>(ovBegin);
            void* pBegin{}; void* pEnd{};
            std::memcpy(&pBegin, record + 0x48, 8);
            std::memcpy(&pEnd, record + 0x50, 8);
            if (!pBegin || pEnd < pBegin) return false;
            auto* param = static_cast<std::byte*>(pBegin);
            void* rule{}; std::memcpy(&rule, param + 0x10, 8);
            if (rule != ruleVa) return false;
            std::uint64_t nameSize{}; std::memcpy(&nameSize, param + 0x28, 8);
            if (nameSize != paramNameLen || nameSize > 15) return false;
            if (std::memcmp(param + 0x18, paramName, paramNameLen) != 0) return false;
            std::memcpy(param + 0x40, &typeWord, 4);
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void* diagRegistry_{}; std::size_t diagCount_{static_cast<std::size_t>(-1)};
    std::uint32_t diagVals_{}; std::string diagName_;
    struct Entry {
        std::string name,description; std::size_t slot{}; std::shared_ptr<message_command_native::Route> route;
        std::string parameterName{"message"}; bool parameterOptional{};
        void* registry{}; void* signature{}; void* failedRegistry{};
        bool hasRaw{}; RawParameter raw{}; std::uint16_t rawSymbol{}; bool rawAllocated{};
        bool hasEnum{}; EnumParameter enumSpec{}; void* enumRegistry{}; std::uint32_t enumIndex{};
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
    static bool registerEnum(void* fn,void* registry,void* enumData,void* values,std::uint64_t& result) noexcept {
        __try { result=reinterpret_cast<std::uint64_t(__fastcall*)(void*,void*,void*)>(fn)(registry,enumData,values); return true; }
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
