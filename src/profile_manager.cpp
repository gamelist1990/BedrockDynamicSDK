#include <bedrock/profile_manager.hpp>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <bedrock/generated_symbols.hpp>
#endif
namespace bedrock {
namespace {
struct FeaturePath {const char* root;const char* child;};
FeaturePath path(Feature f){
    switch(f){
    case Feature::Keyboard:return {"native_keyboard",nullptr};
    case Feature::Actors:return {"native_actors",nullptr};
    case Feature::Items:return {"native_items",nullptr};
    case Feature::PlayerIdentity:return {"native_player_identity",nullptr};
    case Feature::World:return {"native_world",nullptr};
    case Feature::Motion:return {"native_motion","state_vector"};
    case Feature::Camera:return {"native_camera","visual"};
    case Feature::PacketSender:return {"native_packets","sender"};
    case Feature::MovementPackets:return {"native_packets","motion"};
    case Feature::TitlePackets:return {"native_packets","title"};
    case Feature::CommandPackets:return {"native_packets","command_request"};
    case Feature::ClientCommands:return {"native_client_commands",nullptr};
    case Feature::Gui:return {"native_gui",nullptr};
    case Feature::WorldRender:return {"native_world_render",nullptr};
    case Feature::WorldEvents:return {"native_world_events",nullptr};
    case Feature::WorldHeight:return {"native_world","height_limits"};
    }
    return {nullptr,nullptr};
}
}
std::string_view featureName(Feature f)noexcept{const auto p=path(f);return p.root?std::string_view{p.root}:std::string_view{"unknown feature"};}
bool RuntimeProfile::supports(const ProfileRequirements& requirements,std::string& error)const{
    try{
        const auto root=nlohmann::json::parse(json_);
        auto expanded=requirements;
        for(const auto f:requirements.features()){
            if(f==Feature::World){expanded.require(Feature::Actors).require(Feature::Items);}
            if(f==Feature::Motion||f==Feature::Camera)expanded.require(Feature::Actors);
        }
        for(const auto f:expanded.features()){
            const auto p=path(f);
            if(!p.root||!root.contains(p.root)||!root.at(p.root).is_object()){error="missing profile capability: "+std::string(featureName(f));return false;}
            const auto& node=root.at(p.root);
            if(node.value("schema",0)!=1||node.value("pointer_size",8)!=8){error="unsupported capability schema: "+std::string(p.root);return false;}
            if(p.child&&(!node.contains(p.child)||!node.at(p.child).is_object())){error=std::string("missing capability layout: ")+p.root+"."+p.child;return false;}
            if((f==Feature::World||f==Feature::WorldHeight)&&node.value("revision",0)<3){error="world adapter requires layout revision >=3";return false;}
            if(f==Feature::Camera&&node.at("visual").value("revision",0)<9){error="camera adapter requires visual revision >=9";return false;}
            const auto candidate=[](const nlohmann::json& value){
                return value.value("status",std::string{})=="candidate"||
                    (value.contains("runtime_verified")&&!value.at("runtime_verified").get<bool>());
            };
            if(candidates_==CandidatePolicy::VerifiedOnly&&(candidate(node)||(p.child&&candidate(node.at(p.child))))){
                error=std::string("candidate capability requires CandidatePolicy::AllowCandidates: ")+p.root;return false;
            }
        }
        error.clear();return true;
    }catch(const std::exception& e){error=std::string("invalid capability metadata: ")+e.what();return false;}
}
bool ProfileManager::prepare(const ProfileRequirements& requirements,const ProfileOptions& options,const Adapter& adapter,std::string& error){
#ifdef _WIN32
    std::lock_guard preparation(prepareMutex_);
    if(!adapter){error="profile adapter is empty";return false;}
    const auto previous=snapshot();
    if(options.updates!=UpdatePolicy::ForceRemote&&previous&&previous->candidatePolicy()==options.candidates&&previous->supports(requirements,error)){
        try{if(adapter(*previous,error)){error.clear();return true;}}catch(const std::exception& e){error=e.what();}catch(...){error="profile adapter threw an unknown exception";}
    }
    std::string json;ProfileChoice choice;GameIdentity identity;std::string failures;
    const auto attempt=[&](const std::string& text,ProfileChoice selected){
        try{
            Resolver resolver;
            if(!resolver.loadJson(text,error)||!resolver.attachWindowsMainModule(identity,error)||!resolver.exactBuild())return false;
            selected.identity=identity;
            auto candidate=std::shared_ptr<const RuntimeProfile>(new RuntimeProfile(text,std::move(selected),std::move(resolver),options.candidates));
            if(!candidate->supports(requirements,error)||!adapter(*candidate,error))return false;
            {std::lock_guard lock(mutex_);current_=std::move(candidate);}
            error.clear();return true;
        }catch(const std::exception& e){error=e.what();return false;}catch(...){error="profile adapter threw an unknown exception";return false;}
    };
    if(options.updates==UpdatePolicy::ForceRemote){
        if(!inspectLoadedGame(identity,error))return false;
        if(refreshSupportedProfile(identity,json,choice,error)&&attempt(json,choice))return true;
        failures="remote: "+error;
    }else{
        if(fetchSupportedProfile(json,choice,error)){
            identity=choice.identity;
            if(attempt(json,choice))return true;
        }else{
            const auto fetchError=error;
            if(!inspectLoadedGame(identity,error))return false;
            error=fetchError;
        }
        failures="selected profile: "+error;
        if(options.updates==UpdatePolicy::RefreshIfNeeded){
            if(refreshSupportedProfile(identity,json,choice,error)&&attempt(json,choice))return true;
            failures+="; refresh: "+error;
        }
    }
    if(options.embeddedFallback){
        try{
            const std::string embedded=generated::kSymbolDatabase;const auto root=nlohmann::json::parse(embedded);
            ProfileChoice selected;selected.version=root.at("target").at("version").get<std::string>();selected.source=ProfileSource::Embedded;selected.path="embedded";
            if(validateDownloadedProfile(embedded,selected,identity,error)&&attempt(embedded,std::move(selected)))return true;
        }catch(const std::exception& e){error=e.what();}
        failures+="; embedded: "+error;
    }
    error=failures;return false;
#else
    (void)requirements;(void)options;(void)adapter;error="native profile loading requires Windows x64";return false;
#endif
}
} // namespace bedrock
