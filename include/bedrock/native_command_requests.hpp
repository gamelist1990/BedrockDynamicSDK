#pragma once
#include <bedrock/profile_json.hpp>
#include <bedrock/command_requests.hpp>
#include <bedrock/detail/native_profile_functions.hpp>
#include <bedrock/hook_runtime.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <map>
#include <utility>
#if _ITERATOR_DEBUG_LEVEL != 0
#error Native command packets require the MSVC Release STL ABI.
#endif

namespace bedrock {
namespace command_packet_native {
// Exact 1.26.60.5 layout from the request constructor, not a polymorphic
// C++ class. Destruction uses our CRT; the game borrows it during send only.
struct Origin {
    std::uint8_t type{}; std::byte padding[7]{};
    std::array<std::uint64_t, 2> uuid{};
    std::string requestId;
    std::int64_t playerId{};
};
struct Packet {
    void* vtable{};
    std::uint64_t priorityReliability{0x100000002};
    std::byte flags[0x20]{};
    std::string command;
    Origin origin;
    std::int32_t version{};
    bool internal{}; std::byte padding[3]{};
    std::int32_t serializationVersion{5}; std::byte tail[4]{};
};
static_assert(sizeof(std::string)==32 && sizeof(Origin)==0x40 && sizeof(Packet)==0xa0);
static_assert(offsetof(Packet, command)==0x30 && offsetof(Packet, origin)==0x50);
static_assert(offsetof(Packet, version)==0x90 && offsetof(Packet, serializationVersion)==0x98);
static_assert(offsetof(Origin, uuid)==8 && offsetof(Origin, requestId)==0x18 && offsetof(Origin, playerId)==0x38);
inline CommandOriginData identity() {
    CommandOriginData result;
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(result.uuid.data()), 16,
                       BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) throw std::runtime_error("BCryptGenRandom failed");
    // UUID identity is opaque to the native adapter; both words are echoed by
    // the server. A full random requestId also separates embedded SDK copies.
    static constexpr char hex[]="0123456789abcdef";
    const auto* bytes=reinterpret_cast<const unsigned char*>(result.uuid.data());
    result.requestId="bds-";
    for (unsigned i=0;i<16;++i) { result.requestId+=hex[bytes[i]>>4]; result.requestId+=hex[bytes[i]&15]; }
    return result;
}
inline bool copyString(const void* value, std::size_t offset, std::string& output, std::size_t maximum) {
    std::size_t size{}, capacity{};
    const auto* p=static_cast<const std::byte*>(value);
    if (!native_profile::read(value,offset+16,size) || !native_profile::read(value,offset+24,capacity) ||
        size>maximum || capacity<size || capacity>0x10000000) return false;
    const void* data=p+offset;
    if (capacity>15 && !native_profile::read(value,offset,data)) return false;
    if (!size) { output.clear(); return true; }
    if (!data) return false;
    output.resize(size); SIZE_T copied{};
    return ReadProcessMemory(GetCurrentProcess(),data,output.data(),size,&copied) && copied==size;
}
inline bool copyVector(const void* value, std::size_t offset, std::size_t stride, std::size_t maximum,
                       std::uintptr_t& begin, std::size_t& count) {
    std::uintptr_t end{},capacity{};
    if (!native_profile::read(value,offset,begin) || !native_profile::read(value,offset+8,end) ||
        !native_profile::read(value,offset+16,capacity) || end<begin || capacity<end ||
        (end-begin)%stride || (capacity-begin)%stride || (end-begin)/stride>maximum ||
        capacity-begin>0x1000000 || (!begin && end)) return false;
    count=(end-begin)/stride; return true;
}
inline bool capture(void* packet, CommandResponse& response) {
    if (!native_profile::read(packet,0x30,response.origin.type) ||
        !native_profile::read(packet,0x38,response.origin.uuid) ||
        !copyString(packet,0x48,response.origin.requestId,128) ||
        !native_profile::read(packet,0x68,response.origin.playerId) ||
        !native_profile::read(packet,0x70,response.outputType) ||
        !native_profile::read(packet,0x98,response.successCount)) return false;
    std::uintptr_t data{},begin{}; std::size_t count{};
    if (!native_profile::read(packet,0x78,data) || !copyVector(packet,0x80,0x40,64,begin,count)) return false;
    response.hasDataSet=data!=0;
    std::size_t budget=65536;
    for (std::size_t i=0;i<count;++i) {
        auto* message=reinterpret_cast<void*>(begin+i*0x40);
        CommandOutputMessage out; std::uint8_t successful{};
        if (!native_profile::read(message,0,successful) || successful>1 ||
            !copyString(message,8,out.id,std::min<std::size_t>(4096,budget))) return false;
        out.successful=successful!=0; budget-=out.id.size();
        std::uintptr_t parameters{}; std::size_t n{};
        if (!copyVector(message,0x28,32,32,parameters,n)) return false;
        for (std::size_t j=0;j<n;++j) {
            std::string parameter;
            if (!copyString(reinterpret_cast<void*>(parameters+j*32),0,parameter,std::min<std::size_t>(4096,budget))) return false;
            budget-=parameter.size(); out.parameters.push_back(std::move(parameter));
        }
        response.messages.push_back(std::move(out));
    }
    return true;
}
}

// Process-lifetime native owner. Configure before load(), call update() on
// LevelTick, stop() before destroying. No Minecraft pointers escape callbacks.
class NativeCommandRequests {
public:
    using LocalHandler=std::function<void(std::string_view)>;
    using Diagnostic=std::function<void(const std::string&)>;
    NativeCommandRequests() : requests_(command_packet_native::identity) {}
    ~NativeCommandRequests() { stop(); }
    CommandRequests& commands() noexcept { return requests_; }
    void onDiagnostic(Diagnostic diagnostic) { diagnostic_=std::move(diagnostic); }
    bool addLocalCommand(std::string name, LocalHandler callback) {
        if (loaded_ || name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")!=std::string::npos || !callback) return false;
        return handlers_.emplace(std::move(name),std::move(callback)).second;
    }
    bool load(const RuntimeProfile& profile, std::string& error) {
        if (loaded_) { error="command requests already loaded"; return false; }
        try {
            if (!profile.resolver().exactBuild()) throw std::runtime_error("exact executable fingerprint required");
            const auto root=bedrock::parseNativeProfile(profile.json());
            const auto& layout=root.at("native_command_requests");
            if (native_profile::number(layout.at("schema"))!=1 || native_profile::number(layout.at("layout_revision"))!=1 ||
                native_profile::number(layout.at("pointer_size"))!=8 ||
                root.at("target").at("sha256").get<std::string>()!=layout.at("module_sha256").get<std::string>())
                throw std::runtime_error("unsupported command packet layout or identity");
            if (!layout.value("runtime_verified",false) && profile.candidatePolicy()!=CandidatePolicy::AllowCandidates)
                throw std::runtime_error("static command transport requires AllowCandidates");
            base_=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            textBegin_=base_+native_profile::number(root.at("target").at("text_rva"));
            textEnd_=textBegin_+native_profile::number(root.at("target").at("text_size"));
            const auto& sender = root.at("native_packets").at("sender");
            senderOffset_=native_profile::number(sender.at("client_packet_sender_offset"));
            senderSlot_=native_profile::number(sender.at("sender_send_to_server_slot"));
            if (senderOffset_!=0x1e0 || senderSlot_!=2) throw std::runtime_error("unsupported sender layout revision");
            // Check the leaf getter AND its displacement before reading the member.
            const auto* getter=static_cast<const unsigned char*>(native_profile::checked(root,sender.at("packet_sender_getter")));
            std::array<unsigned char,8> leaf{};
            if (!native_profile::read(getter,0,leaf) || leaf!=std::array<unsigned char,8>{0x48,0x8b,0x81,0xe0,1,0,0,0xc3})
                throw std::runtime_error("sender getter ABI mismatch");
            native_profile::checked(root,layout.at("request_constructor"));
            requestVtable_=base_+native_profile::number(layout.at("request_vtable_rva"));
            outputVtable_=base_+native_profile::number(layout.at("output_vtable_rva"));
            auto* requestId=native_profile::checked(root,layout.at("request_get_id"));
            auto* outputId=native_profile::checked(root,layout.at("output_get_id"));
            void* slot{};
            if (!native_profile::read(reinterpret_cast<void*>(requestVtable_),8,slot) || slot!=requestId ||
                !native_profile::read(reinterpret_cast<void*>(outputVtable_),8,slot) || slot!=outputId)
                throw std::runtime_error("command packet vtable mismatch");
            const auto readRva=native_profile::number(layout.at("output_read").at("rva"));
            auto* readTarget=reinterpret_cast<void*>(base_+readRva);
            if (!BDS_HookRuntimeSupports(BDS_HOOK_COMMAND_OUTPUT_READ))
                throw std::runtime_error("loaded hook owner predates 4.5.0; restart Minecraft with current DLLs");
            if (!BDS_HookOwnsTarget(BDS_HOOK_COMMAND_OUTPUT_READ,readTarget)) {
                if (native_profile::checked(root,layout.at("output_read"))!=readTarget || !functionStart(readTarget))
                    throw std::runtime_error("command output decoder boundary mismatch");
            }
            if (!native_profile::read(reinterpret_cast<void*>(outputVtable_),16*8,slot) || slot!=readTarget ||
                !BDS_HookSubscribe(BDS_HOOK_COMMAND_OUTPUT_READ,readTarget,onOutput,this,&readToken_))
                throw std::runtime_error("shared command output subscription failed");
            loaded_=true; note("CommandRequest(77)/CommandOutput(79) adapter loaded [static candidate, runtime unverified]");
            error.clear(); return true;
        } catch (const std::exception& e) { error=e.what(); return false; }
    }
    void update(void* client, void* level) {
        std::lock_guard lock(updateMutex_);
        if (stopped_ || !loaded_) return;
        const auto current=GetCurrentThreadId();
        if (!owner_) owner_=current;
        if (owner_!=current) return;
        if (client!=client_ || level!=level_) {
            sender_=nullptr; requests_.reset();
            { std::lock_guard queueLock(queueMutex_); local_.clear(); }
            client_=client; level_=level;
        }
        observeSender(client);
        std::deque<std::pair<std::string,std::string>> local;
        { std::lock_guard queueLock(queueMutex_); local.swap(local_); }
        if (dropped_.exchange(false)) note("local command queue full or unreadable; command was not sent");
        for (auto& [name,line] : local) try { handlers_.at(name)(line); } catch (...) { note("local command handler failed"); }
        requests_.poll(sender_ ? CommandRequests::Transport{[this](const CommandRequest& request) { return send(request); }} : CommandRequests::Transport{});
    }
    void stop() {
        stopped_=true;
        if (sendToken_) BDS_HookUnsubscribe(std::exchange(sendToken_,nullptr));
        if (readToken_) BDS_HookUnsubscribe(std::exchange(readToken_,nullptr));
        std::lock_guard lock(updateMutex_); sender_=nullptr; requests_.reset();
    }
private:
    bool functionStart(void* target) const {
        const auto address=reinterpret_cast<std::uintptr_t>(target);
        if (address<textBegin_ || address>=textEnd_) return false;
        DWORD64 image{}; const auto* entry=RtlLookupFunctionEntry(address,&image,nullptr);
        return entry && image+entry->BeginAddress==address;
    }
    void observeSender(void* client) {
        sender_=nullptr; void* sender{}; void* vtable{}; void* target{};
        if (!client || !level_ || !native_profile::read(client,senderOffset_,sender) || !sender ||
            !native_profile::read(sender,0,vtable) || !native_profile::read(vtable,senderSlot_*8,target)) return;
        if (sendToken_ && target!=sendTarget_) return;
        if (!sendToken_) {
            if (!BDS_HookOwnsTarget(BDS_HOOK_PACKET_SEND_TO_SERVER,target) && !functionStart(target)) return;
            if (!BDS_HookSubscribe(BDS_HOOK_PACKET_SEND_TO_SERVER,target,onSend,this,&sendToken_)) return;
            sendTarget_=target; note("local text command interception and request sender ready");
        }
        sender_=sender;
    }
    bool send(const CommandRequest& request) {
        if (stopped_ || owner_!=GetCurrentThreadId() || !sender_) return false;
        command_packet_native::Packet packet;
        packet.vtable=reinterpret_cast<void*>(requestVtable_); packet.command=request.command;
        packet.origin.type=request.origin.type; packet.origin.uuid=request.origin.uuid;
        packet.origin.requestId=request.origin.requestId; packet.origin.playerId=request.origin.playerId;
        packet.version=request.version ? request.version : observedVersion_.load(); packet.internal=request.internal;
        // Use middleware so observers in other Mods also see SDK requests.
        BDS_PacketSendArgs args{sender_.load(),&packet};
        BDS_HookDispatch(BDS_HOOK_PACKET_SEND_TO_SERVER,&args);
        return true; // delivery success is reported ONLY by a correlated output
    }
    static void BDS_HOOK_CALL onSend(void* user, void* raw, BDS_HookNext next, void* frame) noexcept {
        auto& self=*static_cast<NativeCommandRequests*>(user);
        const auto& args=*static_cast<BDS_PacketSendArgs*>(raw);
        if (self.stopped_ || args.sender!=self.sender_.load()) { next(frame,raw); return; }
        std::uintptr_t vt{};
        if (!native_profile::read(args.packet,0,vt) || vt!=self.requestVtable_) { next(frame,raw); return; }
        try {
            std::string line;
            if (!command_packet_native::copyString(args.packet,0x30,line,2048)) { next(frame,raw); return; }
            std::int32_t version{};
            if (native_profile::read(args.packet,0x90,version)) self.observedVersion_=version;
            const auto nameEnd=line.find_first_of(" \t");
            auto name=line.substr(0,nameEnd);
            if (!name.empty() && name.front()=='/') name.erase(0,1);
            if (!self.handlers_.contains(name)) { next(frame,raw); return; }
            std::lock_guard lock(self.queueMutex_);
            if (self.local_.size()>=64) { self.dropped_=true; return; }
            self.local_.emplace_back(std::move(name),std::move(line));
        } catch (...) { self.dropped_=true; }
    }
    static void BDS_HOOK_CALL onOutput(void* user, void* raw, BDS_HookNext next, void* frame) noexcept {
        next(frame,raw); // preserve all native decode behavior and unrelated results
        auto& self=*static_cast<NativeCommandRequests*>(user);
        const auto& args=*static_cast<BDS_CommandOutputReadArgs*>(raw);
        std::uint8_t success{}; std::uintptr_t vt{};
        if (self.stopped_ || args.result!=args.output ||
            !native_profile::read(args.output,0x40,success) || success!=1 ||
            !native_profile::read(args.packet,0,vt) || vt!=self.outputVtable_) return;
        try {
            CommandResponse response;
            if (command_packet_native::capture(args.packet,response)) self.requests_.receive(std::move(response));
            else self.dropped_=true;
        } catch (...) { self.dropped_=true; }
    }
    void note(const std::string& text) { if (diagnostic_) diagnostic_(text); }
    CommandRequests requests_;
    Diagnostic diagnostic_;
    std::map<std::string,LocalHandler> handlers_;
    std::mutex queueMutex_,updateMutex_;
    std::deque<std::pair<std::string,std::string>> local_;
    std::atomic<void*> sender_{};
    std::atomic<std::int32_t> observedVersion_{};
    std::atomic<bool> stopped_{},dropped_{};
    void* client_{}; void* level_{}; void* sendTarget_{};
    std::uintptr_t base_{},textBegin_{},textEnd_{},requestVtable_{},outputVtable_{};
    std::uint32_t senderOffset_{},senderSlot_{};
    DWORD owner_{};
    BDS_HookToken sendToken_{},readToken_{};
    bool loaded_{};
};
} // namespace bedrock
