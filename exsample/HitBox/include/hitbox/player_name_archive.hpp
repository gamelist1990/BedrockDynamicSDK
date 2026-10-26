#pragma once
#include "name_tags.hpp"
#include <unordered_map>
namespace hitbox {
struct PlayerUuid {std::uint64_t low{},high{};bool operator==(const PlayerUuid&)const=default;};
struct PlayerUuidHash {std::size_t operator()(PlayerUuid id)const noexcept{return static_cast<std::size_t>(id.low^(id.high+0x9e3779b97f4a7c15ULL+(id.low<<6)+(id.low>>2)));}};
struct CopiedPlayerName {PlayerUuid uuid;std::uint64_t uniqueId{};std::array<char,NameMailbox::maximumBytes> bytes{};std::size_t size{};};
// Only copied PlayerList values enter here. Durable entries are UUID keyed;
// RuntimeIDs/ActorUniqueIDs are aliases belonging to ONE world epoch only.
class PlayerNameArchive {
public:
    void publish(std::uint64_t epoch,const CopiedPlayerName* values,std::size_t count,std::chrono::minutes lifetime) noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);if(!lock)return;
        pendingEpoch_=epoch;pendingCount_=std::min(count,pending_.size());lifetime_=lifetime;
        std::copy_n(values,pendingCount_,pending_.data());pendingReady_=true;
    }
    // Workers own all strings, hashing, expiry and allocations.
    void update() {std::lock_guard lock(mutex_);updateLocked();}
    std::optional<NameMailbox::Value> read(NameKey key,std::uint64_t uniqueId) {
        std::lock_guard lock(mutex_);updateLocked();
        if(key.epoch!=epoch_)return {};
        if(runtimeIds_.size()>4096)runtimeIds_.clear();
        auto alias=runtimeIds_.find(key.runtimeId);
        if(uniqueId){const auto found=uniqueIds_.find(uniqueId);
            if(found!=uniqueIds_.end()) alias=runtimeIds_.insert_or_assign(key.runtimeId,found->second).first;}
        if(alias==runtimeIds_.end())return {};
        const auto found=names_.find(alias->second);
        if(found==names_.end()||Clock::now()>=found->second.expires||found->second.text.empty())return {};
        NameMailbox::Value value;value.size=std::min(value.bytes.size(),found->second.text.size());
        std::memcpy(value.bytes.data(),found->second.text.data(),value.size);value.revision=found->second.revision;value.observedAt=found->second.expires-lifetime_;return value;
    }
    void remember(NameKey key,std::string_view text,Clock::time_point observedAt) {
        if(text.empty())return;std::lock_guard lock(mutex_);updateLocked();
        if(key.epoch!=epoch_)return;const auto alias=runtimeIds_.find(key.runtimeId);if(alias==runtimeIds_.end())return;
        auto& entry=names_[alias->second];if(entry.text!=text){entry.text=text;entry.revision=++revision_;}
        entry.expires=std::max(entry.expires,observedAt+lifetime_);
    }
private:
    struct Entry {std::string text;Clock::time_point expires{};std::uint64_t revision{};};
    void updateLocked() {
        const auto now=Clock::now();
        if(pendingReady_) {
            if(epoch_!=pendingEpoch_){epoch_=pendingEpoch_;runtimeIds_.clear();}
            uniqueIds_.clear();
            for(std::size_t i=0;i<pendingCount_;++i){const auto& item=pending_[i];
                if(!item.uuid.low&&!item.uuid.high)continue;
                if(item.uniqueId)uniqueIds_.insert_or_assign(item.uniqueId,item.uuid);
                const auto text=normalizedName(std::string_view(item.bytes.data(),item.size));
                if(text.empty())continue; // Hidden names never erase or extend old entries.
                auto& entry=names_[item.uuid];if(entry.text!=text){entry.text=text;entry.revision=++revision_;}
                entry.expires=now+lifetime_;
            }
            pendingReady_=false;
        }
        if(now>=nextExpiry_) {
            std::erase_if(names_,[&](const auto& pair){return now>=pair.second.expires;});
            while(names_.size()>1024){const auto oldest=std::min_element(names_.begin(),names_.end(),[](const auto& a,const auto& b){return a.second.expires<b.second.expires;});names_.erase(oldest);}
            nextExpiry_=now+std::chrono::seconds(1);
        }
    }
    std::mutex mutex_;
    std::array<CopiedPlayerName,512> pending_{};
    std::size_t pendingCount_{};std::uint64_t pendingEpoch_{},epoch_{},revision_{};
    bool pendingReady_{};std::chrono::minutes lifetime_{10};Clock::time_point nextExpiry_{};
    std::unordered_map<PlayerUuid,Entry,PlayerUuidHash> names_;
    std::unordered_map<std::uint64_t,PlayerUuid> uniqueIds_,runtimeIds_;
};
}
