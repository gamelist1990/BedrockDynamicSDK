#include <bedrock/sdk/resolver.hpp>
#include <bedrock/profiles/resolver.hpp>
#include <bedrock/profiles/profile_manager.hpp>
#include "profile_facade.hpp"

namespace bedrock::sdk {

struct SymbolResolver::Impl {
    bedrock::Resolver resolver;
};

struct SymbolResolverRuntimeAccess {
    static const bedrock::Resolver& resolver(const SymbolResolver& publicResolver);
};

const bedrock::Resolver& SymbolResolverRuntimeAccess::resolver(const SymbolResolver& publicResolver) {
    return publicResolver.impl_->resolver;
}

SymbolResolver::SymbolResolver() : impl_(std::make_unique<Impl>()) {}
SymbolResolver::~SymbolResolver() = default;

bool SymbolResolver::open(const Profile& profile, std::string& error) {
    auto runtime = ProfileRuntimeAccess::runtime(profile);
    if (!runtime) { error = "profile runtime unavailable"; return false; }
    impl_->resolver = runtime->resolver();
    if (!impl_->resolver.exactBuild()) { error = "profile does not match the running exact build"; return false; }
    return true;
}

bool SymbolResolver::openFixture(std::string_view profileJson, std::string& error) {
    impl_->resolver = {};
    return impl_->resolver.loadJson(profileJson, error);
}

bool SymbolResolver::exactBuild() const noexcept { return impl_->resolver.exactBuild(); }

std::optional<SymbolResolution> SymbolResolver::resolve(std::string_view name) const {
    const auto resolution = impl_->resolver.resolve(name);
    if (!resolution) return std::nullopt;
    return SymbolResolution{0, resolution->address,
                            resolution->exact_build, resolution->verified};
}

} // namespace bedrock::sdk
