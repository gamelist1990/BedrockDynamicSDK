#pragma once

#include <bedrock/sdk/profiles.hpp>
#include <bedrock/profiles/profile_manager.hpp>
#include <memory>

namespace bedrock::sdk {

struct ProfileRuntimeAccess {
    static std::shared_ptr<const bedrock::RuntimeProfile> runtime(const Profile& profile) noexcept;
};

} // namespace bedrock::sdk
