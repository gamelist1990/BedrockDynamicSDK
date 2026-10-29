#pragma once
#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/native/client_commands.hpp>
#include <bedrock/commands/command_queue.hpp>
#include <bedrock/runtime/hook_runtime.h>
#include <atomic>
#include <limits>

namespace bedrock {
// Process-lifetime owner, like GameHooks. Define commands and callbacks before
// load(). Call update()/dispatch() on LevelTick; reset() from WorldChange.
// Packet callbacks only copy a bounded command string and enqueue typed values.
class NativeCommandService {
public:
    using Diagnostic = std::function<void(const std::string&)>;
    using Rejection = std::function<void(CommandSubmission)>;
    ~NativeCommandService() { stop(); if (packetToken_) BDS_HookUnsubscribe(packetToken_); }
    void onDiagnostic(Diagnostic handler) { diagnostic_ = std::move(handler); }
    void onRejected(Rejection handler) { rejection_ = std::move(handler); }
    bool add(CommandDefinition definition) {
        if (loaded_ || queue_.contains(definition.name().value())) return false;
        const auto name = definition.name().value();
        if (!native_.add(name, definition.description().value(),
                         {definition.argument().value(), definition.optional()},
                         [this](const NativeCommandInvocation& command) {
            const auto result = queue_.submit(command.name, command.integer, CommandSource::Native);
            report(result);
            return result == CommandSubmission::Queued || result == CommandSubmission::PairedDuplicate;
        })) return false;
        return queue_.add(std::move(definition));
    }
    bool load(const RuntimeProfile& profile, std::string& error) {
        loaded_ = false;
        if (!profile.supports({Feature::ClientCommands, Feature::CommandPackets, Feature::PacketSender}, error)) return false;
        try {
            const auto root = bedrock::parseNativeProfile(profile.json());
            base_ = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            textBegin_ = base_ + number(root.at("target").at("text_rva"));
            textEnd_ = textBegin_ + number(root.at("target").at("text_size"));
            const auto& packets = root.at("native_packets");
            senderOffset_ = number(packets.at("sender").at("client_packet_sender_offset"));
            senderSlot_ = number(packets.at("sender").at("sender_send_to_server_slot"));
            const auto& command = packets.at("command_request");
            commandVtable_ = base_ + number(command.at("vtable_rva"));
            commandOffset_ = number(command.at("command_offset"));
            if (senderOffset_ > 65536 || senderSlot_ > 4096 || commandOffset_ > 4096)
                throw std::runtime_error("invalid command transport layout");
            loaded_ = native_.load(profile, error);
            return loaded_;
        } catch (const std::exception& e) { error = e.what(); return false; }
    }
    void reset(std::uint64_t epoch) {
        queue_.reset(epoch); sender_.store(nullptr);
        resetRequested_.store(true); warnings_.store(0);
    }

    void stop() { stopped_.store(true); sender_.store(nullptr); queue_.stop(); }
    void update(void* client, std::uint64_t epoch) {
        if (stopped_.load() || !loaded_) return;
        DWORD expected{};
        ownerThread_.compare_exchange_strong(expected, GetCurrentThreadId());
        if (ownerThread_.load() != GetCurrentThreadId()) return;
        if (epoch_ != epoch) reset(epoch);
        if (resetRequested_.exchange(false) || epoch_ != epoch) {
            native_.reset(); ticks_ = 0;
            lastRegistration_ = NativeClientCommands::Result::unavailable;
            epoch_ = epoch;
        }
        observeSender(client);
        if (ticks_++ % 20 == 0) {
            const auto result = native_.update(client);
            if (result == NativeClientCommands::Result::registered)
                note("native CommandRegistry registered [autocomplete + local execution]");
            else if (result != lastRegistration_ && result == NativeClientCommands::Result::collision)
                note("native command name collision; existing metadata preserved; packet fallback active");
            else if (result != lastRegistration_ && result == NativeClientCommands::Result::failed)
                note("native command registration failed: " + native_.error());
            lastRegistration_ = result;
        }
        const auto warnings = warnings_.exchange(0);
        if (rejection_) {
            if (warnings & 1) rejection_(CommandSubmission::InvalidArguments);
            if (warnings & 2) rejection_(CommandSubmission::Full);
        }
    }
    std::size_t dispatch(std::size_t maximum = 1) {
        if (stopped_.load() || ownerThread_.load() != GetCurrentThreadId()) return 0;
        return queue_.drain(maximum);
    }
    bool pending() const { return queue_.pending(); }
private:
    static std::uint32_t number(const nlohmann::json& value) {
        const auto result = value.is_string() ? std::stoull(value.get<std::string>(), nullptr, 0) : value.get<std::uint64_t>();
        if (result > UINT32_MAX) throw std::runtime_error("command layout overflow");
        return static_cast<std::uint32_t>(result);
    }
    // MSVC Release x64 string ABI, shared with native command registration.
    struct Line { char bytes[2049]{}; std::size_t size{}; };
    static bool copyLine(const void* value, Line& out) noexcept {
        if (!value) return false;
        __try {
            const auto* p = static_cast<const std::byte*>(value);
            std::size_t size{}, capacity{};
            std::memcpy(&size, p + 16, 8); std::memcpy(&capacity, p + 24, 8);
            if (size > 2048 || capacity < size || capacity > 0x1000000) return false;
            const char* data = static_cast<const char*>(value);
            if (capacity > 15) std::memcpy(&data, p, 8);
            if (!data) return false;
            std::memcpy(out.bytes, data, size); out.size = size; return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static void BDS_HOOK_CALL onPacket(void* context, void* raw, BDS_HookNext next, void* frame) noexcept {
        auto& self = *static_cast<NativeCommandService*>(context);
        const auto& args = *static_cast<BDS_PacketSendArgs*>(raw);
        if (self.stopped_.load() || !args.sender || args.sender != self.sender_.load()) { next(frame, raw); return; }
        std::uintptr_t vtable{}; Line line;
        if (!command_native::read(args.packet, 0, vtable) || vtable != self.commandVtable_ ||
            !copyLine(static_cast<const std::byte*>(args.packet) + self.commandOffset_, line)) { next(frame, raw); return; }
        try {
            const auto result = self.queue_.submit(std::string_view{line.bytes, line.size}, CommandSource::Packet);
            if (result == CommandSubmission::UnknownCommand) { next(frame, raw); return; }
            self.report(result);
        } catch (...) { self.warnings_.fetch_or(2); }
    }
    void observeSender(void* client) {
        sender_.store(nullptr);
        void* sender{}; void** vtable{}; void* target{};
        if (!command_native::read(client, senderOffset_, sender) || !sender ||
            !command_native::read(sender, 0, vtable) ||
            !command_native::read(vtable, senderSlot_ * sizeof(void*), target)) { sender_.store(nullptr); return; }
        const auto address = reinterpret_cast<std::uintptr_t>(target);
        if (address < textBegin_ || address >= textEnd_ || (packetToken_ && target != installedTarget_)) {
            sender_.store(nullptr); return;
        }
        if (!packetToken_) {
            if (!BDS_HookOwnsTarget(BDS_HOOK_PACKET_SEND_TO_SERVER, target)) {
                DWORD64 image{};
                const auto* function = RtlLookupFunctionEntry(address, &image, nullptr);
                if (!function || image + function->BeginAddress != address) return;
            }
            if (!BDS_HookSubscribe(BDS_HOOK_PACKET_SEND_TO_SERVER, target, onPacket, this, &packetToken_)) return;
            installedTarget_ = target;
            note("local command packet fallback ready [unknown commands forwarded]");
        }
        sender_.store(sender);
    }
    void report(CommandSubmission result) {
        if (result == CommandSubmission::InvalidArguments) warnings_.fetch_or(1);
        else if (result == CommandSubmission::Full) warnings_.fetch_or(2);
    }
    void note(const std::string& text) { if (diagnostic_) diagnostic_(text); }
    CommandQueue queue_;
    NativeClientCommands native_;
    Diagnostic diagnostic_;
    Rejection rejection_;
    std::atomic<void*> sender_{};
    std::atomic<bool> stopped_{};
    std::atomic<unsigned> warnings_{};
    std::atomic<DWORD> ownerThread_{};
    std::uintptr_t base_{}, textBegin_{}, textEnd_{}, commandVtable_{};
    std::uint32_t senderOffset_{}, senderSlot_{}, commandOffset_{};
    BDS_HookToken packetToken_{};
    void* installedTarget_{};
    std::uint64_t epoch_{};
    std::atomic<bool> resetRequested_{};
    unsigned ticks_{};
    NativeClientCommands::Result lastRegistration_{NativeClientCommands::Result::unavailable};
    bool loaded_{};
};
} // namespace bedrock
