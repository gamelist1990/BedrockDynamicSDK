#include <bedrock/sdk/packet_deferral.hpp>

#ifdef _WIN32
#include <bedrock/generated/symbols.hpp>
#include <bedrock/native/packet_deferral.hpp>
#include <bedrock/profiles/profile_json.hpp>
#endif

#include <sstream>

namespace bedrock::sdk {

struct PacketDeferral::Impl {
#ifdef _WIN32
    bedrock::NativePacketDeferral native;
#endif
};

PacketDeferral::PacketDeferral() : impl_(std::make_unique<Impl>()) {}
PacketDeferral::~PacketDeferral() = default;
PacketDeferral::PacketDeferral(PacketDeferral&&) noexcept = default;
PacketDeferral& PacketDeferral::operator=(PacketDeferral&&) noexcept = default;

bool PacketDeferral::configure(const Profile& profile, std::string& error,
                               std::vector<std::string>& warnings, PacketDeferralPolicy policy) {
#ifdef _WIN32
    warnings.clear();
    const auto runtime = ProfileRuntimeAccess::runtime(profile);
    if (!runtime) {
        error = "packet deferral profile runtime snapshot unavailable";
        return false;
    }
    try {
        std::string deferralError;
        if (!impl_->native.install(*runtime, bedrock::generated::kSymbolDatabase, policy,
                                   deferralError)) {
            error = deferralError;
            return false;
        }
        std::ostringstream line;
        line << "packet deferral ready (handlers=";
        for (unsigned index = 0; index < impl_->native.handlerCount(); ++index) {
            if (index) line << ' ';
            line << impl_->native.handlerLabel(index) << "=0x" << std::hex
                 << impl_->native.handlerRva(index) << std::dec;
        }
        line << "; held packets replayed on the game thread)";
        warnings.push_back(line.str());
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
#else
    (void)profile;
    (void)warnings;
    (void)policy;
    error = "packet deferral is unavailable on this host";
    return false;
#endif
}

bool PacketDeferral::ready() const noexcept {
#ifdef _WIN32
    return impl_->native.ready();
#else
    return false;
#endif
}

void PacketDeferral::setEnabled(bool enabled) noexcept {
#ifdef _WIN32
    impl_->native.setEnabled(enabled);
#else
    (void)enabled;
#endif
}

bool PacketDeferral::enabled() const noexcept {
#ifdef _WIN32
    return impl_->native.enabled();
#else
    return false;
#endif
}

unsigned PacketDeferral::pump(unsigned budget) noexcept {
#ifdef _WIN32
    return impl_->native.pump(budget);
#else
    (void)budget;
    return 0;
#endif
}

unsigned PacketDeferral::clear() noexcept {
#ifdef _WIN32
    return impl_->native.clear();
#else
    return 0;
#endif
}

unsigned PacketDeferral::queueDepth() const noexcept {
#ifdef _WIN32
    return impl_->native.queueDepth();
#else
    return 0;
#endif
}

PacketDeferralStatistics PacketDeferral::takeStatistics() noexcept {
    PacketDeferralStatistics out;
#ifdef _WIN32
    out.ready = impl_->native.ready();
    out.handlers = impl_->native.handlerCount();
    const auto raw = impl_->native.takeStatistics();
    out.calls = static_cast<unsigned>(raw.calls);
    out.deferred = static_cast<unsigned>(raw.deferred);
    out.replayed = static_cast<unsigned>(raw.replayed);
    out.overflow = static_cast<unsigned>(raw.overflow);
    out.otherObject = static_cast<unsigned>(raw.otherObject);
    out.otherClass = static_cast<unsigned>(raw.otherClass);
    out.worldDrops = static_cast<unsigned>(raw.worldDrops);
    out.agedOut = static_cast<unsigned>(raw.agedOut);
    out.queue = raw.queue;
    static_assert(out.byHandler.size() == raw.byHandler.size());
    for (unsigned i = 0; i < out.handlers; ++i) {
        const auto& handler = raw.byHandler[i];
        out.byHandler[i] = {handler.packetId, handler.calls, handler.deferred, handler.replayed};
    }
#endif
    return out;
}

std::string PacketDeferral::handlerSummary() const {
#ifdef _WIN32
    std::ostringstream line;
    for (unsigned index = 0; index < impl_->native.handlerCount(); ++index) {
        if (index) line << ' ';
        line << impl_->native.handlerLabel(index) << "=0x" << std::hex
             << impl_->native.handlerRva(index) << std::dec;
    }
    return line.str();
#else
    return {};
#endif
}

} // namespace bedrock::sdk
