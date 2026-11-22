#include "bedrock/sdk/packet_wire.hpp"

#include "profile_facade.hpp"

#ifdef _WIN32
#include <bedrock/packets/wire_packet.hpp>
#include <bedrock/packets/wire_packet_events.hpp>
#include <bedrock/profiles/profile_json.hpp>
#include <bedrock/runtime/hook_runtime.h>

#include <windows.h>

#include <string>
#endif

namespace bedrock::sdk {

struct PacketWire::Impl {
#ifdef _WIN32
    bedrock::PacketWireEvents events;
    Handler handler{};
    void* user{};
    void* sendTarget{};
    BDS_HookToken wireToken{};
    BDS_HookToken sendToken{};

    // Translate the native bridge event into the value copy the Mod sees, let
    // the handler decide, and splice any payload edit back into the borrowed
    // frame.
    void deliver(bedrock::PacketWireSendEvent& native) {
        PacketWireEvent event;
        event.sender = native.sender();
        event.packetId = static_cast<int>(native.id());
        event.bytes.assign(native.packet().bytes());
        event.readable = true;
        if (handler) handler(event, user);
        if (event.cancelled) {
            native.cancel();
            return;
        }
        if (event.bytes == native.packet().bytes()) return;
        // Re-parse so a malformed rewrite is rejected instead of corrupting
        // the game's frame; the original header is always preserved.
        const bedrock::WirePacket edited(event.bytes);
        native.packet().replacePayload(edited.payload());
    }

    // Attribution only: the runtime needs a subscriber on the sender's
    // send-to-server entry to bind the synchronous wire event to that sender.
    static void BDS_HOOK_CALL sendHook(void*, void* raw, BDS_HookNext next, void* frame) noexcept {
        next(frame, raw);
    }

    static void BDS_HOOK_CALL wireHook(void* user, void* raw, BDS_HookNext next,
                                       void* frame) noexcept {
        auto& self = *static_cast<Impl*>(user);
        auto& args = *static_cast<BDS_PacketWireArgs*>(raw);
        bool delivered = false;
        try {
            delivered = self.events.dispatch(
                args, [&self](bedrock::PacketWireSendEvent& native) { self.deliver(native); },
                next, frame);
        } catch (...) {
            delivered = false;
        }
        if (delivered) return;
        // The native string could not be copied safely, or the copy is not a
        // valid frame. Report an unreadable event so the Mod can apply its own
        // policy, then forward the untouched frame unless it asked to drop it.
        PacketWireEvent event;
        event.sender = args.sender;
        event.packetId = -1;
        event.readable = false;
        if (self.handler) self.handler(event, self.user);
        if (!event.cancelled) next(frame, raw);
    }
#endif
};

PacketWire::PacketWire() : impl_(std::make_unique<Impl>()) {}
PacketWire::~PacketWire() { stop(); }
PacketWire::PacketWire(PacketWire&&) noexcept = default;
PacketWire& PacketWire::operator=(PacketWire&&) noexcept = default;

bool PacketWire::load(const Profile& profile, std::string& error) {
#ifdef _WIN32
    const auto runtime = ProfileRuntimeAccess::runtime(profile);
    if (!runtime) {
        error = "packet wire profile runtime snapshot unavailable";
        return false;
    }
    try {
        impl_->events.load(bedrock::parseNativeProfile(runtime->json()));
    } catch (const std::exception& e) {
        error = std::string("packet wire bridge unavailable: ") + e.what();
        return false;
    }
    if (!impl_->events.target()) {
        error = "packet wire bridge target unresolved";
        return false;
    }
    error.clear();
    return true;
#else
    (void)profile;
    error = "packet wire unavailable on this host";
    return false;
#endif
}

void* PacketWire::wireTarget() const noexcept {
#ifdef _WIN32
    return impl_->events.target();
#else
    return nullptr;
#endif
}

bool PacketWire::accepts(void* sendTarget) const noexcept {
#ifdef _WIN32
    if (!sendTarget || !impl_->events.target()) return false;
    if (BDS_HookOwnsTarget(BDS_HOOK_PACKET_SEND_TO_SERVER, sendTarget)) return true;
    DWORD64 image{};
    const auto* entry =
        RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(sendTarget), &image, nullptr);
    return entry && image + entry->BeginAddress == reinterpret_cast<DWORD64>(sendTarget);
#else
    (void)sendTarget;
    return false;
#endif
}

bool PacketWire::install(void* sendTarget, Handler handler, void* user, std::string& error) {
#ifdef _WIN32
    if (!impl_->events.target()) {
        error = "packet wire bridge is not loaded";
        return false;
    }
    if (!handler) {
        error = "packet wire handler is missing";
        return false;
    }
    if (!accepts(sendTarget)) {
        error = "packet send target is not a hookable exact-build function entry";
        return false;
    }
    impl_->handler = handler;
    impl_->user = user;
    impl_->sendTarget = sendTarget;
    if (!impl_->wireToken &&
        !impl_->events.subscribe(&Impl::wireHook, impl_.get(), &impl_->wireToken)) {
        error = "packet wire bridge hook registration failed";
        return false;
    }
    if (!impl_->sendToken &&
        !BDS_HookSubscribe(BDS_HOOK_PACKET_SEND_TO_SERVER, sendTarget, &Impl::sendHook,
                           impl_.get(), &impl_->sendToken)) {
        BDS_HookUnsubscribe(impl_->wireToken);
        impl_->wireToken = nullptr;
        error = "packet send attribution hook registration failed";
        return false;
    }
    error.clear();
    return true;
#else
    (void)sendTarget;
    (void)handler;
    (void)user;
    error = "packet wire unavailable on this host";
    return false;
#endif
}

void PacketWire::stop() noexcept {
#ifdef _WIN32
    if (impl_->sendToken) BDS_HookUnsubscribe(impl_->sendToken);
    if (impl_->wireToken) BDS_HookUnsubscribe(impl_->wireToken);
    impl_->sendToken = nullptr;
    impl_->wireToken = nullptr;
    impl_->sendTarget = nullptr;
    impl_->handler = nullptr;
    impl_->user = nullptr;
#endif
}

bool PacketWire::installed() const noexcept {
#ifdef _WIN32
    return impl_->wireToken && impl_->sendToken;
#else
    return false;
#endif
}

} // namespace bedrock::sdk
