#include <bedrock/sdk/profiles.hpp>

#include <bedrock/profiles/profile_manager.hpp>
#include "profile_facade.hpp"

#include <filesystem>
#include <memory>
#include <utility>

namespace bedrock::sdk {

struct Profile::Impl {
    std::shared_ptr<const bedrock::RuntimeProfile> runtime;
};

struct Profiles::Impl {
    bedrock::ProfileManager manager;
    std::shared_ptr<const bedrock::RuntimeProfile> snapshot;
};

std::shared_ptr<const bedrock::RuntimeProfile> ProfileRuntimeAccess::runtime(const Profile& profile) noexcept {
    return profile.impl_ ? profile.impl_->runtime : std::shared_ptr<const bedrock::RuntimeProfile>{};
}

Profile::Profile(std::string json) : json_(std::move(json)) {}
Profile::Profile(std::string json, std::shared_ptr<Impl> impl)
    : json_(std::move(json)), impl_(std::move(impl)) {}
const std::string& Profile::json() const noexcept { return json_; }
std::string Profile::version() const noexcept {
    if (impl_ && impl_->runtime) return impl_->runtime->choice().version;
    return {};
}

Profiles::Profiles() : impl_(std::make_unique<Impl>()) {}
Profiles::~Profiles() = default;
bool Profiles::open(ProfileUpdatePolicy updatePolicy, ProfileCandidatePolicy candidatePolicy, std::string& error) {
    bedrock::ProfileOptions options;
    if (updatePolicy == ProfileUpdatePolicy::EmbeddedOnly) options.updates = bedrock::UpdatePolicy::EmbeddedOnly;
    else if (updatePolicy == ProfileUpdatePolicy::ForceRemote) options.updates = bedrock::UpdatePolicy::ForceRemote;
    else options.updates = bedrock::UpdatePolicy::RefreshIfNeeded;
    options.candidates = candidatePolicy == ProfileCandidatePolicy::AllowCandidates
        ? bedrock::CandidatePolicy::AllowCandidates
        : bedrock::CandidatePolicy::VerifiedOnly;
    const bool managerOpened = impl_->manager.open({}, options, error);
    if (!managerOpened) return false;
    impl_->snapshot = impl_->manager.snapshot();
    return static_cast<bool>(impl_->snapshot);
}
bool Profiles::open(ProfileUpdatePolicy updatePolicy, std::string& error) {
    bedrock::ProfileOptions options;
    if (updatePolicy == ProfileUpdatePolicy::EmbeddedOnly) options.updates = bedrock::UpdatePolicy::EmbeddedOnly;
    else if (updatePolicy == ProfileUpdatePolicy::ForceRemote) options.updates = bedrock::UpdatePolicy::ForceRemote;
    else options.updates = bedrock::UpdatePolicy::RefreshIfNeeded;
    // Default candidate policy is VerifiedOnly
    const bool managerOpened = impl_->manager.open({}, options, error);
    if (!managerOpened) return false;
    impl_->snapshot = impl_->manager.snapshot();
    return static_cast<bool>(impl_->snapshot);
}
Profile Profiles::snapshot() const {
    if (!impl_->snapshot) return Profile(std::string{});
    auto impl = std::make_shared<Profile::Impl>();
    impl->runtime = impl_->snapshot;
    return Profile(impl_->snapshot->json(), impl);
}

} // namespace bedrock::sdk