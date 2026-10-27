#pragma once
#include <bedrock/api_types.hpp>
#include <bedrock/remote_catalog.hpp>
#include <bedrock/resolver.hpp>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
namespace bedrock {
enum class Feature { Input, Gameplay, LevelTick, NameTags, Fov, Keyboard, Actors, Items, PlayerIdentity, World, Motion, Camera,
    PacketSender, MovementPackets, TitlePackets, CommandPackets, ClientCommands, Gui, WorldRender, WorldEvents, WorldHeight, Brightness, GameType };
std::string_view featureName(Feature feature) noexcept;
class ProfileRequirements {
    std::vector<Feature> features_;
public:
    ProfileRequirements()=default;
    ProfileRequirements(std::initializer_list<Feature> features){for(auto f:features)require(f);}
    ProfileRequirements& require(Feature feature){if(std::find(features_.begin(),features_.end(),feature)==features_.end())features_.push_back(feature);return *this;}
    const std::vector<Feature>& features()const noexcept{return features_;}
};
struct ProfileOptions {
    UpdatePolicy updates=UpdatePolicy::RefreshIfNeeded;
    CandidatePolicy candidates=CandidatePolicy::VerifiedOnly;
    bool embeddedFallback=true;
};
// Only ProfileManager can construct a validated immutable snapshot. Existing
// snapshots stay alive while a new one is acquired; active hooks are not patched.
class RuntimeProfile {
    std::string json_;ProfileChoice choice_;Resolver resolver_;CandidatePolicy candidates_;
    RuntimeProfile(std::string json,ProfileChoice choice,Resolver resolver,CandidatePolicy policy)
        :json_(std::move(json)),choice_(std::move(choice)),resolver_(std::move(resolver)),candidates_(policy){}
    friend class ProfileManager;
public:
    const std::string& json()const noexcept{return json_;}
    const ProfileChoice& choice()const noexcept{return choice_;}
    const Resolver& resolver()const noexcept{return resolver_;}
    CandidatePolicy candidatePolicy()const noexcept{return candidates_;}
    bool supports(const ProfileRequirements& requirements,std::string& error)const;
};
class ProfileManager {
public:
    // Adapter runs synchronously on this initialization thread. It must validate
    // its required layouts/entry guards before installing hooks. Failure triggers
    // a throttled GitHub refresh and then an exact-build embedded fallback.
    using Adapter=std::function<bool(const RuntimeProfile&,std::string&)>;
    bool prepare(const ProfileRequirements& requirements,const ProfileOptions& options,const Adapter& adapter,std::string& error);
    bool open(const ProfileRequirements& requirements,const ProfileOptions& options,std::string& error){
        return prepare(requirements,options,[](const RuntimeProfile&,std::string&){return true;},error);
    }
    std::shared_ptr<const RuntimeProfile> snapshot()const{std::lock_guard lock(mutex_);return current_;}
private:
    std::mutex prepareMutex_;
    mutable std::mutex mutex_;
    std::shared_ptr<const RuntimeProfile> current_;
};
} // namespace bedrock
