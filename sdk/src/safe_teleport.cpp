#include "bedrock/sdk/safe_teleport.hpp"

#include "profile_facade.hpp"

#ifdef _WIN32
#include <bedrock/native/safe_teleport.hpp>
#endif

namespace bedrock::sdk {

struct SafeTeleport::Impl {
#ifdef _WIN32
    bedrock::NativeSafeTeleport native;
#endif
};

#ifdef _WIN32
struct SafeTeleport::Scope::NativeScope {
    bedrock::NativeSafeTeleport::Scope scope;
    explicit NativeScope(bedrock::NativeSafeTeleport::Scope&& value)
        : scope(std::move(value)) {}
};
#else
struct SafeTeleport::Scope::NativeScope {};
#endif

SafeTeleport::Scope::~Scope() = default;
SafeTeleport::Scope::Scope(std::unique_ptr<NativeScope> native)
    : native_(std::move(native)) {}
SafeTeleport::Scope::Scope(Scope&&) noexcept = default;
SafeTeleport::Scope& SafeTeleport::Scope::operator=(Scope&&) noexcept = default;

SafeTeleport::SafeTeleport() : impl_(std::make_unique<Impl>()) {}
SafeTeleport::~SafeTeleport() = default;
SafeTeleport::SafeTeleport(SafeTeleport&&) noexcept = default;
SafeTeleport& SafeTeleport::operator=(SafeTeleport&&) noexcept = default;

bool SafeTeleport::load(const Profile& profile, std::string& error) {
#ifdef _WIN32
    const auto runtime = ProfileRuntimeAccess::runtime(profile);
    if (!runtime) {
        error = "safe teleport profile runtime snapshot unavailable";
        return false;
    }
    return impl_->native.load(*runtime, error);
#else
    (void)profile;
    error = "safe teleport unavailable on this host";
    return false;
#endif
}

void SafeTeleport::reset(std::uint64_t epoch) {
#ifdef _WIN32
    impl_->native.reset(epoch);
#else
    (void)epoch;
#endif
}

void SafeTeleport::stop() {
#ifdef _WIN32
    impl_->native.stop();
#endif
}

SafeTeleport::Scope SafeTeleport::tick(void* client, void* level, std::uint64_t epoch) {
#ifdef _WIN32
    auto nativeScope = impl_->native.tick(client, level, epoch);
    if (!nativeScope)
        return Scope{};
    return Scope{std::make_unique<Scope::NativeScope>(std::move(nativeScope))};
#else
    (void)client;
    (void)level;
    (void)epoch;
    return Scope{};
#endif
}

bool SafeTeleport::message(const std::string& text) const {
#ifdef _WIN32
    return impl_->native.message(text);
#else
    (void)text;
    return false;
#endif
}

} // namespace bedrock::sdk
