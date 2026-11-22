#pragma once
#include <memory>
#include <string>
namespace bedrock { class RuntimeProfile; }
namespace bedrock::sdk {
enum class ProfileUpdatePolicy { EmbeddedOnly, RefreshIfNeeded, ForceRemote };
enum class ProfileCandidatePolicy { VerifiedOnly, AllowCandidates };
class Profile { public:
 const std::string& json() const noexcept; std::string version() const noexcept;
 private:
  struct Impl;
  explicit Profile(std::string json);
  explicit Profile(std::string json, std::shared_ptr<Impl> impl);
  friend class Profiles;
  friend struct ProfileRuntimeAccess;
  std::string json_;
  std::shared_ptr<Impl> impl_; 
};
class Profiles { public:
 Profiles(); ~Profiles(); Profiles(const Profiles&) = delete; Profiles& operator=(const Profiles&) = delete;
 bool open(ProfileUpdatePolicy updatePolicy, ProfileCandidatePolicy candidatePolicy, std::string& error);
 bool open(ProfileUpdatePolicy updatePolicy, std::string& error); Profile snapshot() const;
 private: struct Impl; std::unique_ptr<Impl> impl_; 
};
struct ProfileRuntimeAccess {
    static std::shared_ptr<const bedrock::RuntimeProfile> runtime(const Profile& profile) noexcept;
};
} // namespace bedrock::sdk