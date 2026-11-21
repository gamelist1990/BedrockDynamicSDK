#pragma once
#include <memory>
#include <string>
namespace bedrock::sdk {
struct ProfileRuntimeAccess;
enum class ProfileUpdatePolicy { EmbeddedOnly, RefreshIfNeeded, ForceRemote };
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
 bool open(ProfileUpdatePolicy updatePolicy, std::string& error); Profile snapshot() const;
 private: struct Impl; std::unique_ptr<Impl> impl_; 
};
}
