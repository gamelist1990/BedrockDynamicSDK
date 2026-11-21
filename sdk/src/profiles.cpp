#include <bedrock/sdk/profiles.hpp>

#include <bedrock/profiles/profile_manager.hpp>
#include <memory>

namespace bedrock::sdk {

struct Profiles::Impl {
    bedrock::ProfileManager manager;
    std::shared_ptr<const bedrock::RuntimeProfile> snapshot;
};

Profile::Profile(std::string json) : json_(std::move(json)) {}
const std::string& Profile::json() const noexcept { return json_; }
std::string Profile::version() const noexcept { return {}; }

Profiles::Profiles() : impl_(std::make_unique<Impl>()) {}
Profiles::~Profiles() = default;
bool Profiles::open(ProfileUpdatePolicy updatePolicy, std::string& error) {
    bedrock::ProfileOptions options;
    if (updatePolicy == ProfileUpdatePolicy::EmbeddedOnly) options.updates = bedrock::UpdatePolicy::EmbeddedOnly;
    else if (updatePolicy == ProfileUpdatePolicy::ForceRemote) options.updates = bedrock::UpdatePolicy::ForceRemote;
    else options.updates = bedrock::UpdatePolicy::RefreshIfNeeded;
    if (!impl_->manager.open({}, options, error)) return false;
    impl_->snapshot = impl_->manager.snapshot();
    return static_cast<bool>(impl_->snapshot);
}
Profile Profiles::snapshot() const {
    return Profile(impl_->snapshot ? impl_->snapshot->json() : std::string{});
}

} // namespace bedrock::sdk
