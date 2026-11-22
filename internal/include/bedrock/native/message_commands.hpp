#pragma once
#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/commands/command_requests.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
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
// Typed parameter kinds for native registration (revision 4 layout).
// String is the registry's type-1 kind: a free-form single token stored
// as std::string. Enum is the same kind with a registry enum descriptor
// attached (value suggestions; the stored value is still the string).
// Bool attaches the registry's Boolean descriptor. A typed entry must
// not mix a message parameter into its overloads: a signature carrying
// a message overload captures every invocation, so message-shaped
// definitions keep the legacy single-message registration.
// String is the registry's "text" kind (type 15): a single-token
// std::string whose token rule accepts numeric tokens too — the
// type-1 kind shared with Enum rejects any token that parses
// entirely as a number. Enum is the type-1 kind with a game enum
// descriptor attached (its values are non-numeric by construction).
enum class NativeMessageParamKind { Int, Float, Bool, String, Enum };
struct NativeMessageParam {
    NativeMessageParamKind kind{NativeMessageParamKind::String};
    std::string name;
    bool optional{};
    std::string enumName; // Enum only: registry descriptor name
};
namespace message_command_native {
struct Route {
    std::string name;
    std::function<void(NativeMessageInvocation)> handler;
    std::atomic<const void*> registry{};
    void *resolve{},*destroy{},*free{};
    std::uintptr_t textBegin{},textEnd{};
    // Typed registration (empty for message routes): the declared
    // overloads, and the kinds actually registered per overload after
    // descriptor degradation (an Enum whose descriptor the registry
    // lacks registers as String; likewise Bool). execute() reads the
    // effective kinds; they are rewritten at every registration, and a
    // command object only ever executes in the session that registered
    // it (its registry pointer is checked first).
    bool typed{};
    std::vector<std::vector<NativeMessageParam>> typedOverloads;
    std::vector<std::vector<NativeMessageParamKind>> effectiveKinds;
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

// Reads an MSVC std::string stored in game memory layout.
inline std::string readStoredString(const void* addr) {
    std::uint64_t size{},cap{};
    std::memcpy(&size,static_cast<const std::byte*>(addr)+0x10,8);
    std::memcpy(&cap,static_cast<const std::byte*>(addr)+0x18,8);
    if (size>4096) return {};
    const char* data=static_cast<const char*>(addr);
    if (cap>=16) std::memcpy(&data,addr,8);
    if (!data) return {};
    return std::string(data,static_cast<std::size_t>(size));
}
// The kinds a route's overload actually parses with (descriptor
// degradation applied at registration; declared kinds before that).
inline const std::vector<NativeMessageParam>& declaredOverload(const Route& route,std::size_t o) {
    static const std::vector<NativeMessageParam> empty;
    return o<route.typedOverloads.size() ? route.typedOverloads[o] : empty;
}
inline NativeMessageParamKind effectiveKind(const Route& route,std::size_t o,std::size_t i) {
    if (o<route.effectiveKinds.size() && i<route.effectiveKinds[o].size()) return route.effectiveKinds[o][i];
    const auto& overload=declaredOverload(route,o);
    return i<overload.size() ? overload[i].kind : NativeMessageParamKind::String;
}
// Typed command object: the native parser writes each argument at the
// field offset its parameter descriptor declares (0x20-byte slots from
// +0x50, positions shared across the route's overloads). Presence is
// sentinel-based (the parser leaves an untouched slot alone): Int
// INT32_MAX, Float NaN bits, Bool 0xCD byte, String/Enum empty string.
// execute() reconstructs the "/name value..." line, so handlers see
// exactly the form the message path delivers.
class TypedCommand {
public:
    static constexpr std::size_t kMaxSlots=8;
    static constexpr std::int32_t kSlotBase=0x50;
    static constexpr std::int32_t kSlotSize=0x20;
    explicit TypedCommand(std::size_t index):routeIndex(index) {
        const auto route=index<routes.size()?routes[index]:nullptr;
        if (!route || route->typedOverloads.empty()) return;
        const auto& first=route->typedOverloads[0];
        for (std::size_t i=0;i<first.size() && i<kMaxSlots;++i) {
            auto* slot=slots.data()+i*kSlotSize;
            switch (first[i].kind) {
                case NativeMessageParamKind::Int: { std::int32_t v=INT32_MAX; std::memcpy(slot,&v,4); break; }
                case NativeMessageParamKind::Float: { std::uint32_t v=0x7fc00000u; std::memcpy(slot,&v,4); break; }
                case NativeMessageParamKind::Bool: slot[0]=std::byte{0xcd}; break;
                case NativeMessageParamKind::String:
                case NativeMessageParamKind::Enum: break; // zeroed = valid empty string
            }
        }
    }
    virtual ~TypedCommand() {
        const auto route=routeIndex<routes.size()?routes[routeIndex]:nullptr;
        if (!route || !route->destroy) return;
        destroyMessage(route->destroy,message.data());
        if (route->typedOverloads.empty()) return;
        const auto& first=route->typedOverloads[0];
        for (std::size_t i=0;i<first.size() && i<kMaxSlots;++i) {
            const auto kind=effectiveKind(*route,0,i);
            if (kind==NativeMessageParamKind::String || kind==NativeMessageParamKind::Enum)
                destroyMessage(route->destroy,slots.data()+i*kSlotSize);
        }
    }
    virtual bool collectOptionalArguments() { return true; }
    virtual void execute(const void* origin,void*) const {
        const auto route=routeIndex<routes.size()?routes[routeIndex]:nullptr;
        if (!route || !registry || route->registry.load()!=registry) return;
        try {
            auto present=[&](std::size_t o,std::size_t i) {
                const auto* slot=slots.data()+i*kSlotSize;
                switch (effectiveKind(*route,o,i)) {
                    case NativeMessageParamKind::Int: { std::int32_t v{}; std::memcpy(&v,slot,4); return v!=INT32_MAX; }
                    case NativeMessageParamKind::Float: { std::uint32_t v{}; std::memcpy(&v,slot,4); return v!=0x7fc00000u; }
                    case NativeMessageParamKind::Bool: return slot[0]!=std::byte{0xcd};
                    case NativeMessageParamKind::String:
                    case NativeMessageParamKind::Enum: { std::uint64_t s{}; std::memcpy(&s,slot+0x10,8); return s!=0; }
                }
                return false;
            };
            // The parser matched one overload; reconstruct against the
            // candidate whose required params are all present, preferring
            // the one with the most present params.
            int best=-1; std::size_t bestScore=0;
            for (std::size_t o=0;o<route->typedOverloads.size();++o) {
                const auto& overload=route->typedOverloads[o];
                bool ok=true; std::size_t score=0;
                for (std::size_t i=0;i<overload.size() && i<kMaxSlots;++i) {
                    const bool p=present(o,i);
                    if (p) ++score;
                    if (!p && !overload[i].optional) ok=false;
                }
                if (ok && (best<0 || score>bestScore)) { best=static_cast<int>(o); bestScore=score; }
            }
            if (best<0) return;
            const auto& overload=route->typedOverloads[static_cast<std::size_t>(best)];
            std::string line="/"+route->name;
            for (std::size_t i=0;i<overload.size() && i<kMaxSlots;++i) {
                if (!present(static_cast<std::size_t>(best),i)) continue;
                const auto* slot=slots.data()+i*kSlotSize;
                line+=' ';
                switch (effectiveKind(*route,static_cast<std::size_t>(best),i)) {
                    case NativeMessageParamKind::Int: { std::int32_t v{}; std::memcpy(&v,slot,4); line+=std::to_string(v); break; }
                    case NativeMessageParamKind::Float: {
                        float v{}; std::memcpy(&v,slot,4);
                        char buf[64]; std::snprintf(buf,sizeof(buf),"%g",static_cast<double>(v)); line+=buf; break; }
                    case NativeMessageParamKind::Bool: line+=slot[0]!=std::byte{0} ? "true" : "false"; break;
                    case NativeMessageParamKind::String:
                    case NativeMessageParamKind::Enum: {
                        std::string value=readStoredString(slot);
                        if (value.find_first_of(" \"\\")==std::string::npos) { line+=value; break; }
                        line+='"';
                        for (const char c : value) { if (c=='"' || c=='\\') line+='\\'; line+=c; }
                        line+='"';
                        break; }
                }
            }
            NativeMessageInvocation invocation;
            invocation.line=std::move(line);
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
        } catch (...) {} // no C++ exceptions across native virtual calls
    }
    int version{}; std::uint32_t padding{};
    const void* registry{};
    int symbol{-1}; std::uint8_t permission{5},reserved{}; std::uint16_t flags{};
    std::uint8_t messageFlags{1}; std::array<std::byte,7> padding2{};
    std::array<std::byte,0x20> message{}; // +0x28
    std::size_t routeIndex{}; // +0x48
    std::array<std::byte,kSlotSize*kMaxSlots> slots{}; // +0x50
};
static_assert(sizeof(TypedCommand)==0x50+TypedCommand::kSlotSize*TypedCommand::kMaxSlots);
template<std::size_t I> void* __fastcall createTyped(void* result) {
    auto* command=new(std::nothrow) TypedCommand(I); std::memcpy(result,&command,8); return result;
}
template<std::size_t... I> constexpr auto typedFactories(std::index_sequence<I...>) {
    return std::array<Factory,sizeof...(I)>{&createTyped<I>...};
}
inline constexpr auto typedFactoryFunctions=typedFactories(std::make_index_sequence<32>{});
}
// Native registration follows Top's SyncLocal command/factory path. It does
// not depend on the server's /me signature, proxy factory, or overload order.
class NativeMessageCommands {
public:
    enum class State { WaitingClient, WaitingRegistry, WaitingTemplate, Registered, Collision, Failed };
    // Routes are process-lifetime slots; clearing ours on destruction
    // keeps a stale handler from firing after the owner is gone (the
    // Command objects the game still holds check the route first).
    ~NativeMessageCommands() { for (auto& entry:entries_) message_command_native::routes[entry.slot]=nullptr; }
    // parameterName is only the label autocomplete shows for the message
    // argument (defaults to "message", as in /me); the parser type is the
    // CommandMessage type regardless of the label.
    bool add(std::string name, std::string description, std::function<void(NativeMessageInvocation)> handler,
             std::string parameterName = "message", bool parameterOptional = false) {
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
        message_command_native::routes[slot]=route;
        entries_.push_back({std::move(name),std::move(description),slot,std::move(route),std::move(parameterName),parameterOptional}); return true;
    }
    // add() after load(): the route is wired with the function pointers
    // and text range load() resolved, and the entry registers on the
    // next update(). This exists so a definition set that grows after
    // startup (a plugin reload that adds a command) can still join the
    // local registry; slots are process-lifetime, as with add().
    bool addLoaded(std::string name, std::string description, std::function<void(NativeMessageInvocation)> handler,
             std::string parameterName = "message", bool parameterOptional = false) {
        if (!loaded_ || !handler || name.empty() || name.size()>64 || description.size()>1024 ||
            description.find('\0')!=std::string::npos ||
            name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-")!=std::string::npos || entries_.size()>=32) return false;
        if (parameterName.empty() || parameterName.size()>15 ||
            parameterName.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")!=std::string::npos) return false;
        for (const auto& entry:entries_) if (entry.name==name) return false;
        if (message_command_native::nextSlot>=32) return false;
        const auto slot=message_command_native::nextSlot++;
        auto route=std::make_shared<message_command_native::Route>();
        route->name=name; route->handler=std::move(handler);
        route->resolve=resolve_; route->destroy=destroy_; route->free=free_;
        route->textBegin=textBegin_; route->textEnd=textEnd_;
        message_command_native::routes[slot]=route;
        entries_.push_back({std::move(name),std::move(description),slot,std::move(route),std::move(parameterName),parameterOptional}); return true;
    }
    // Typed variant of add(): the overloads register with real
    // parameter descriptors, so autocomplete renders typed parameters
    // and the parser validates/converts values; the handler still
    // receives a reconstructed "/name value..." invocation line.
    // Every overload needs 1..8 parameters; names longer than the
    // parameter string's inline capacity (15) ride in heap form and
    // are copied by the game during registration.
    bool addTyped(std::string name, std::string description, std::vector<std::vector<NativeMessageParam>> overloads,
             std::function<void(NativeMessageInvocation)> handler) {
        if (loaded_) return false;
        return addTypedImpl(std::move(name),std::move(description),std::move(overloads),std::move(handler),false);
    }
    bool addTypedLoaded(std::string name, std::string description, std::vector<std::vector<NativeMessageParam>> overloads,
             std::function<void(NativeMessageInvocation)> handler) {
        if (!loaded_) return false;
        return addTypedImpl(std::move(name),std::move(description),std::move(overloads),std::move(handler),true);
    }
    bool load(const RuntimeProfile& profile, std::string& error) {
        loaded_=false;
        try {
            if (!profile.resolver().exactBuild() || profile.candidatePolicy()!=CandidatePolicy::AllowCandidates)
                throw std::runtime_error("message command registration requires an exact build and AllowCandidates");
            const auto root=parseNativeProfile(profile.json());
            const auto& layout=root.at("native_client_commands").at("message_forwarding");
            if (layout.value("revision",0)!=4 || layout.at("module_sha256")!=root.at("target").at("sha256"))
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
            finalize_=native_profile::checked(root,layout.at("finalize_overload"));
            paramCopy_=native_profile::checked(root,layout.at("param_copy"));
            paramReserve_=native_profile::checked(root,layout.at("param_reserve"));
            intType_=reinterpret_cast<void*>(base_+native_profile::number(layout.at("integer_type_rva")));
            intRule_=reinterpret_cast<void*>(base_+native_profile::number(layout.at("integer_rule_rva")));
            floatType_=reinterpret_cast<void*>(base_+native_profile::number(layout.at("float_type_rva")));
            floatRule_=reinterpret_cast<void*>(base_+native_profile::number(layout.at("float_rule_rva")));
            stringType_=reinterpret_cast<void*>(base_+native_profile::number(layout.at("string_type_rva")));
            stringRule_=reinterpret_cast<void*>(base_+native_profile::number(layout.at("string_rule_rva")));
            enumType_=reinterpret_cast<void*>(base_+native_profile::number(layout.at("enum_type_rva")));
            enumRule_=reinterpret_cast<void*>(base_+native_profile::number(layout.at("enum_rule_rva")));
            descArrayOffset_=static_cast<std::uint32_t>(native_profile::number(layout.at("descriptor_array_offset")));
            descArrayEndOffset_=static_cast<std::uint32_t>(native_profile::number(layout.at("descriptor_array_end_offset")));
            descStride_=static_cast<std::uint32_t>(native_profile::number(layout.at("descriptor_stride")));
            descRuleOffset_=static_cast<std::uint32_t>(native_profile::number(layout.at("descriptor_rule_offset")));
            if (descStride_!=0x48 || descRuleOffset_!=0x28)
                throw std::runtime_error("unsupported CommandRegistry descriptor layout revision");
            message_command_native::Command probe(0);
            if (reinterpret_cast<const std::byte*>(&probe.message)-reinterpret_cast<const std::byte*>(&probe)!=0x28)
                throw std::runtime_error("compiler CommandMessage offset mismatch");
            HMODULE pinned{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(message_command_native::factoryFunctions[0]),&pinned))
                throw std::runtime_error("command provider pin failed");
            textBegin_=base_+native_profile::number(root.at("target").at("text_rva"));
            textEnd_=textBegin_+native_profile::number(root.at("target").at("text_size"));
            for (auto& entry:entries_) {
                entry.route->resolve=resolve_; entry.route->destroy=destroy_; entry.route->free=free_;
                entry.route->textBegin=textBegin_;
                entry.route->textEnd=textEnd_;
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
            if (entry.typed) {
                const auto typedState=registerTyped(entry,registry);
                if (typedState!=State::Registered) return typedState;
                continue;
            }
            // CommandParameterData is built per entry so each command can
            // label its message argument (the name lives in the parameter).
            alignas(8) std::array<std::byte,0x78> parameter{};
            const auto put=[&](std::size_t offset,const auto& value) { std::memcpy(parameter.data()+offset,&value,sizeof(value)); };
            put(0,type); put(8,rule_);
            std::memcpy(parameter.data()+0x18,entry.parameterName.data(),entry.parameterName.size());
            put(0x28,static_cast<std::uint64_t>(entry.parameterName.size())); put(0x30,std::uint64_t{15});
            put(0x40,std::int32_t{-1}); put(0x50,std::int32_t{-1});
            put(0x58,std::int32_t{0x28}); put(0x5c,std::int32_t{-1});
            // Same optional flag the integer adapter uses (+0x60): the
            // parser then accepts the command with no token and renders
            // the argument as [name: string] in autocomplete.
            if (entry.parameterOptional) parameter[0x60]=std::byte{1};
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
        std::string parameterName{"message"}; bool parameterOptional{};
        void* registry{}; void* signature{}; void* failedRegistry{};
        bool typed{};
    };
    bool addTypedImpl(std::string name, std::string description, std::vector<std::vector<NativeMessageParam>> overloads,
             std::function<void(NativeMessageInvocation)> handler, bool wireRoute) {
        if (!handler || name.empty() || name.size()>64 || description.size()>1024 ||
            description.find('\0')!=std::string::npos ||
            name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-")!=std::string::npos || entries_.size()>=32) return false;
        if (overloads.empty() || overloads.size()>8) return false;
        for (const auto& overload : overloads) {
            if (overload.empty() || overload.size()>message_command_native::TypedCommand::kMaxSlots) return false;
            for (const auto& param : overload) {
                if (param.name.empty() || param.name.size()>64 ||
                    param.name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")!=std::string::npos) return false;
                if (param.kind==NativeMessageParamKind::Enum && param.enumName.empty()) return false;
            }
        }
        for (const auto& entry:entries_) if (entry.name==name) return false;
        if (message_command_native::nextSlot>=32) return false;
        const auto slot=message_command_native::nextSlot++;
        auto route=std::make_shared<message_command_native::Route>();
        route->name=name; route->handler=std::move(handler);
        route->typed=true; route->typedOverloads=std::move(overloads);
        if (wireRoute) {
            route->resolve=resolve_; route->destroy=destroy_; route->free=free_;
            route->textBegin=textBegin_; route->textEnd=textEnd_;
        }
        message_command_native::routes[slot]=route;
        Entry entry;
        entry.name=std::move(name); entry.description=std::move(description); entry.slot=slot;
        entry.route=std::move(route); entry.typed=true;
        entries_.push_back(std::move(entry));
        return true;
    }
    struct DescriptorInfo { std::size_t index{}; const void* elem{}; const char* nameData{}; };
    // The registry's enum descriptor array (per-registry; rebuilt when
    // a session changes, so discovery runs at every registration).
    std::map<std::string,DescriptorInfo> discoverDescriptors(void* registry) const {
        std::map<std::string,DescriptorInfo> found;
        void *begin{},*end{};
        if (!native_profile::read(registry,descArrayOffset_,begin) ||
            !native_profile::read(registry,descArrayEndOffset_,end) || !begin || !end) return found;
        const auto count=(reinterpret_cast<std::uintptr_t>(end)-reinterpret_cast<std::uintptr_t>(begin))/descStride_;
        if (count>8192) return found;
        for (std::size_t i=0;i<count;++i) {
            const auto* elem=static_cast<const std::byte*>(begin)+i*descStride_;
            const std::string name=message_command_native::readStoredString(elem);
            if (name.empty()) continue;
            std::uint64_t cap{};
            std::memcpy(&cap,elem+0x18,8);
            const char* data=reinterpret_cast<const char*>(elem);
            if (cap>=16) std::memcpy(&data,elem,8);
            found[name]={i,elem,data};
        }
        return found;
    }
    using ParamImage=std::array<std::byte,0x78>;
    // Builds one CommandParameterData image. Enum/Bool parameters whose
    // descriptor the registry lacks degrade to String; the effective
    // kind is reported through effectiveKind.
    ParamImage buildTypedParam(const NativeMessageParam& param, std::int32_t offset,
                               const std::map<std::string,DescriptorInfo>& descriptors,
                               NativeMessageParamKind& effectiveKind,
                               std::vector<char*>& scratch) const {
        ParamImage image{};
        const auto put=[&](std::size_t off,const auto& value) { std::memcpy(image.data()+off,&value,sizeof(value)); };
        std::uint16_t type{};
        const void* rule{};
        effectiveKind=param.kind;
        switch (param.kind) {
            case NativeMessageParamKind::Int:
                native_profile::read(intType_,0,type); rule=intRule_; break;
            case NativeMessageParamKind::Float:
                native_profile::read(floatType_,0,type); rule=floatRule_; break;
            case NativeMessageParamKind::String:
                native_profile::read(stringType_,0,type); rule=stringRule_; break;
            case NativeMessageParamKind::Enum: {
                const auto it=descriptors.find(param.enumName);
                if (it==descriptors.end()) {
                    // Degrades to the text kind (accepts any token).
                    effectiveKind=NativeMessageParamKind::String;
                    native_profile::read(stringType_,0,type); rule=stringRule_; break;
                }
                native_profile::read(enumType_,0,type); rule=enumRule_;
                put(0x38,it->second.nameData);
                put(0x40,static_cast<std::int32_t>(0x300000|static_cast<std::int32_t>(it->second.index)));
                put(0x54,std::int32_t{1});
                break; }
            case NativeMessageParamKind::Bool: {
                const auto it=descriptors.find("Boolean");
                if (it==descriptors.end()) {
                    effectiveKind=NativeMessageParamKind::String;
                    native_profile::read(stringType_,0,type); rule=stringRule_; break;
                }
                type=2;
                rule=static_cast<const std::byte*>(it->second.elem)+descRuleOffset_;
                break; }
        }
        put(0,type); put(8,rule);
        if (param.name.size()<=15) {
            std::memcpy(image.data()+0x18,param.name.data(),param.name.size());
            put(0x28,static_cast<std::uint64_t>(param.name.size())); put(0x30,std::uint64_t{15});
        } else {
            // Heap form: the game's parameter copy constructor reads
            // (pointer, size, capacity) and allocates its own copy, so
            // this buffer only has to live until registration returns;
            // the caller owns it through the scratch list.
            char* heap=new char[param.name.size()+1];
            std::memcpy(heap,param.name.data(),param.name.size()+1);
            scratch.push_back(heap);
            put(0x18,static_cast<void*>(heap));
            put(0x28,static_cast<std::uint64_t>(param.name.size()));
            put(0x30,static_cast<std::uint64_t>(param.name.size()));
        }
        // +0x40 is the enum word: set by the Enum case when a descriptor
        // attached; every other kind (and a degraded Enum) keeps -1.
        if (effectiveKind!=NativeMessageParamKind::Enum) {
            std::int32_t minusOne=-1;
            std::memcpy(image.data()+0x40,&minusOne,4);
        }
        put(0x50,std::int32_t{-1});
        put(0x58,offset); put(0x5c,std::int32_t{-1});
        if (param.optional) image[0x60]=std::byte{1};
        return image;
    }
    bool callAppendParam(void* overload, const ParamImage& param) noexcept {
        __try {
            void *begin{},*end{},*cap{};
            std::memcpy(&begin,static_cast<std::byte*>(overload)+0x48,8);
            std::memcpy(&end,static_cast<std::byte*>(overload)+0x50,8);
            std::memcpy(&cap,static_cast<std::byte*>(overload)+0x58,8);
            if (end==cap) {
                std::uint64_t newCap=static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(end)-reinterpret_cast<std::uintptr_t>(begin))/0x78+1;
                reinterpret_cast<void(__fastcall*)(void*,std::uint64_t*)>(paramReserve_)(static_cast<std::byte*>(overload)+0x48,&newCap);
                std::memcpy(&end,static_cast<std::byte*>(overload)+0x50,8);
                std::memcpy(&cap,static_cast<std::byte*>(overload)+0x58,8);
                if (end==cap) return false;
            }
            reinterpret_cast<void(__fastcall*)(void*,const void*)>(paramCopy_)(end,param.data());
            auto* newEnd=static_cast<std::byte*>(end)+0x78;
            std::memcpy(static_cast<std::byte*>(overload)+0x50,&newEnd,8);
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool callFinalizeOverload(void* registry, void* signature, void* overload) noexcept {
        __try { reinterpret_cast<void(__fastcall*)(void*,void*,void*)>(finalize_)(registry,signature,overload); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void* findOurTypedOverload(void* signature, std::size_t slot) const noexcept {
        std::uintptr_t begin{},end{};
        if (!native_profile::read(signature,0x40,begin) || !begin ||
            !native_profile::read(signature,0x48,end)) return nullptr;
        const auto count=(end-begin)/0x80;
        void* found{};
        for (std::size_t i=0;i<count && i<64;++i) {
            auto* overload=reinterpret_cast<void*>(begin+i*0x80);
            void* table{}; message_command_native::Factory factory{};
            if (native_profile::read(overload,8,table) && table==factoryTable_ &&
                native_profile::read(overload,16,factory) && factory==message_command_native::typedFactoryFunctions[slot])
                found=overload;
        }
        return found;
    }
    bool ownsTypedOverload(void* signature, std::size_t slot) const noexcept {
        return findOurTypedOverload(signature,slot)!=nullptr;
    }
    State registerTyped(Entry& entry, void* registry) {
        void* existing{};
        if (!lookup(find_,registry,&entry.name,&existing)) { error_="CommandRegistry lookup failed: "+entry.name; return State::Failed; }
        if (existing) {
            if (entry.registry==registry && entry.signature==existing && ownsTypedOverload(existing,entry.slot)) return State::Registered;
            error_="command name already exists; preserved: /"+entry.name; return State::Collision;
        }
        if (entry.failedRegistry==registry) { error_="previous registration failed: /"+entry.name; return State::Failed; }
        const auto descriptors=discoverDescriptors(registry);
        if (!registerName(register_,registry,&entry.name,entry.description.c_str())) {
            entry.failedRegistry=registry; error_="native typed registration failed: /"+entry.name; return State::Failed;
        }
        struct ScratchGuard {
            std::vector<char*> buffers;
            ~ScratchGuard() { for (char* p : buffers) delete[] p; }
        };
        ScratchGuard scratch;
        std::vector<std::vector<NativeMessageParamKind>> effective;
        for (const auto& overload : entry.route->typedOverloads) {
            std::vector<ParamImage> images;
            std::vector<NativeMessageParamKind> kinds;
            for (std::size_t i=0;i<overload.size();++i) {
                NativeMessageParamKind kind{};
                images.push_back(buildTypedParam(overload[i],
                    static_cast<std::int32_t>(message_command_native::TypedCommand::kSlotBase+
                        static_cast<std::int32_t>(i)*message_command_native::TypedCommand::kSlotSize),
                    descriptors,kind,scratch.buffers));
                kinds.push_back(kind);
            }
            effective.push_back(std::move(kinds));
            alignas(8) std::array<std::byte,64> callable{};
            auto factory=message_command_native::typedFactoryFunctions[entry.slot];
            std::memcpy(callable.data(),&factoryTable_,8); std::memcpy(callable.data()+8,&factory,8);
            if (!addOverload(overload_,registry,entry.name.c_str(),callable.data(),images[0].data())) {
                entry.failedRegistry=registry; error_="native typed registration failed: /"+entry.name; return State::Failed;
            }
            void* signature{};
            if (!lookup(find_,registry,&entry.name,&signature) || !signature) {
                entry.failedRegistry=registry; error_="native typed registration failed: /"+entry.name; return State::Failed;
            }
            if (images.size()>1) {
                void* own=findOurTypedOverload(signature,entry.slot);
                bool ok=own!=nullptr;
                for (std::size_t i=1;ok && i<images.size();++i) ok=callAppendParam(own,images[i]);
                if (ok) ok=callFinalizeOverload(registry,signature,own);
                if (!ok) {
                    entry.failedRegistry=registry; error_="native typed registration failed: /"+entry.name; return State::Failed;
                }
            }
        }
        void* created{};
        if (!lookup(find_,registry,&entry.name,&created) || !created || !ownsTypedOverload(created,entry.slot)) {
            entry.failedRegistry=registry; error_="native typed registration failed: /"+entry.name; return State::Failed;
        }
        entry.route->effectiveKinds=std::move(effective);
        entry.registry=registry; entry.signature=created; entry.route->registry=registry;
        return State::Registered;
    }
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
    std::uintptr_t textBegin_{},textEnd_{};
    std::uint32_t clientMinecraft_{},minecraftCommands_{},commandsRegistry_{};
    void *register_{},*find_{},*overload_{},*factoryTable_{},*resolve_{},*destroy_{},*free_{},*type_{},*rule_{};
    void *finalize_{},*paramCopy_{},*paramReserve_{};
    void *intType_{},*intRule_{},*floatType_{},*floatRule_{},*stringType_{},*stringRule_{},*enumType_{},*enumRule_{};
    std::uint32_t descArrayOffset_{},descArrayEndOffset_{},descStride_{},descRuleOffset_{};
    bool loaded_{};
    std::string error_;
};
} // namespace bedrock
