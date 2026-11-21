#pragma once
#include <memory>
#include <string>
namespace bedrock::sdk {
enum class ProfileUpdatePolicy { EmbeddedOnly, RefreshIfNeeded, ForceRemote };
class Profile { public:
 const std::string& json() const noexcept; std::string version() const noexcept;
 private: explicit Profile(std::string json); friend class Profiles; std::string json_;
};
class Profiles { public:
 Profiles(); ~Profiles(); Profiles(const Profiles&) = delete; Profiles& operator=(const Profiles&) = delete;
 bool open(ProfileUpdatePolicy updatePolicy, std::string& error); Profile snapshot() const;
 private: struct Impl; std::unique_ptr<Impl> impl_; 
};
}
