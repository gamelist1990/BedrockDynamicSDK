#include <bedrock/profile_json.hpp>
#include <bedrock/profile_manager.hpp>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <bedrock/hook_runtime.h>
#endif
#ifdef _WIN32
#include <bedrock/generated_symbols.hpp>
#endif
namespace bedrock {
namespace {
struct FeaturePath {const char* root;const char* child;};
FeaturePath path(Feature f){
    switch(f){
    case Feature::Input:case Feature::Gameplay:case Feature::LevelTick:case Feature::NameTags:case Feature::Fov:return {"symbols",nullptr};
    case Feature::Keyboard:return {"native_keyboard",nullptr};
    case Feature::Actors:return {"native_actors",nullptr};
    case Feature::Items:return {"native_items",nullptr};
    case Feature::PlayerIdentity:return {"native_player_identity",nullptr};
    case Feature::World:return {"native_world",nullptr};
    case Feature::Motion:return {"native_motion","state_vector"};
    case Feature::Camera:case Feature::Brightness:case Feature::GameType:return {"native_camera","visual"};
    case Feature::PacketSender:return {"native_packets","sender"};
    case Feature::PacketWire:case Feature::MovementPackets:return {"layouts","PacketWire"};
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
std::string_view featureName(Feature f)noexcept{
    switch(f){case Feature::Input:return "input hooks";case Feature::Gameplay:return "gameplay hooks";
    case Feature::LevelTick:return "level tick hook";case Feature::NameTags:return "name tag hook";case Feature::Fov:return "FOV hook";default:break;}
    switch(f){
    case Feature::Keyboard:return "GameCore::keyboardEvent";
    case Feature::Actors:return "Actor";
    case Feature::Items:return "ItemStack";
    case Feature::PlayerIdentity:return "LocalPlayer";
    case Feature::World:case Feature::WorldHeight:return "BlockSource";
    case Feature::Motion:return "StateVectorComponent";
    case Feature::Camera:case Feature::Brightness:return "Camera";
    case Feature::GameType:return "Player::setPlayerGameType";
    case Feature::PacketSender:return "PacketSender::sendToServer";
    case Feature::PacketWire:case Feature::MovementPackets:return "Packet wire bridge";
    case Feature::TitlePackets:return "SetTitlePacket";
    case Feature::CommandPackets:return "CommandRequestPacket";
    case Feature::ClientCommands:return "CommandRegistry";
    case Feature::Gui:return "GuiData::displayClientMessage";
    case Feature::WorldRender:return "LevelRenderer";
    case Feature::WorldEvents:return "ClientInstance world events";
    default:return "unknown feature";
    }
}
bool RuntimeProfile::supports(const ProfileRequirements& requirements,std::string& error)const{
    try{
        const auto root=bedrock::parseNativeProfile(json_);
        auto expanded=requirements;
        for(const auto f:requirements.features()){
            if(f==Feature::World){expanded.require(Feature::Actors).require(Feature::Items);}
            if(f==Feature::Motion||f==Feature::Camera||f==Feature::GameType)expanded.require(Feature::Actors);
        }
        for(const auto f:expanded.features()){
            const auto p=path(f);
            if(!p.root||!root.contains(p.root)||!root.at(p.root).is_object()){error="missing Minecraft layout: "+std::string(featureName(f))+" (function entry verification does not establish object layout)";return false;}
            const auto& node=root.at(p.root);
            if(f==Feature::ClientCommands&&!node.contains("layout")){
                error="CommandRegistry local-command object layout unavailable (message forwarding metadata only)";return false;
            }
#ifdef _WIN32
            const auto hook=[&](const char* name,BDS_HookPoint point){
                const auto target=resolveSharedHook(resolver_,name,point);
                if(!target||!isUsableSharedHookResolution(*target)||
                   (candidates_==CandidatePolicy::VerifiedOnly&&!target->verified)){
                    error=std::string("required hook unavailable: ")+name;return false;
                }
                return true;
            };
            switch(f){
            case Feature::Input:
                if(!hook("Keyboard::feed",BDS_HOOK_KEYBOARD_FEED)||!hook("MouseDevice::feed",BDS_HOOK_MOUSE_FEED))return false;break;
            case Feature::Gameplay:
                if(!hook("ClientInstance::grabCursor",BDS_HOOK_GRAB_CURSOR)||!hook("ClientInstance::releaseCursor",BDS_HOOK_RELEASE_CURSOR))return false;break;
            case Feature::LevelTick:if(!hook("MultiPlayerLevel::_subTick",BDS_HOOK_LEVEL_SUB_TICK))return false;break;
            case Feature::NameTags:if(!hook("Actor::getNameTag",BDS_HOOK_ACTOR_GET_NAME_TAG))return false;break;
            case Feature::Fov:if(!hook("LevelRendererPlayer::getFov",BDS_HOOK_GET_FOV))return false;break;
            case Feature::Keyboard:if(!hook("GameCore::keyboardEvent",BDS_HOOK_GAMECORE_KEYBOARD_EVENT))return false;break;
            case Feature::WorldEvents: {
                const auto names=node.value("available_events",nlohmann::json::array({"onPlayerLoaded","onLevelExit","onDimensionChangedEvent","onCancelJoinGame"}));
                for(const auto& name:names) {
                    const auto value=name.get<std::string>();
                    if(value=="onPlayerLoaded") {if(!hook("ClientInstance::onPlayerLoaded",BDS_HOOK_CLIENT_PLAYER_LOADED))return false;}
                    else if(value=="onLevelExit") {if(!hook("ClientInstance::onLevelExit",BDS_HOOK_CLIENT_LEVEL_EXIT))return false;}
                    else if(value=="onDimensionChangedEvent") {if(!hook("ClientInstance::onDimensionChangedEvent",BDS_HOOK_CLIENT_DIMENSION_CHANGED))return false;}
                    else if(value=="onCancelJoinGame") {if(!hook("ClientInstance::onCancelJoinGame",BDS_HOOK_CLIENT_JOIN_CANCELLED))return false;}
                    else {error="unsupported native world event: "+value;return false;}
                }
                break;
            }
            default:break;
            }
#endif
            if(std::string_view{p.root}=="symbols")continue;

            if(p.child&&(!node.contains(p.child)||!node.at(p.child).is_object())){error=std::string("missing capability layout: ")+p.root+"."+p.child;return false;}
            const auto& schemaNode=std::string_view{p.root}=="layouts"?node.at(p.child):node;
            if(schemaNode.value("schema",0)!=1||schemaNode.value("pointer_size",8)!=8){error="unsupported capability schema: "+std::string(p.root);return false;}
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
    bool embeddedTried=false;
    std::string embeddedFailure;
    const auto tryEmbedded=[&]{
        embeddedTried=true;
        try{
            const std::string embedded=generated::kSymbolDatabase;const auto root=bedrock::parseNativeProfile(embedded);
            ProfileChoice selected;selected.version=root.at("target").at("version").get<std::string>();selected.source=ProfileSource::Embedded;selected.path="embedded";
            if(validateDownloadedProfile(embedded,selected,identity,error)&&attempt(embedded,std::move(selected)))return true;
        }catch(const std::exception& e){error=e.what();}
        embeddedFailure=error;return false;
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
        // A cache can match the EXE yet predate this Mod's required layouts.
        // Prefer the bundled exact-build definition before a throttled fetch.
        if(options.embeddedFallback&&choice.source!=ProfileSource::Embedded&&tryEmbedded())return true;
        if(options.updates==UpdatePolicy::RefreshIfNeeded){
            if(refreshSupportedProfile(identity,json,choice,error)&&attempt(json,choice))return true;
            failures+="; refresh: "+error;
        }
    }
    if(options.embeddedFallback){
        if(!embeddedTried&&tryEmbedded())return true;
        failures+="; embedded: "+embeddedFailure;
    }
    error=failures;return false;
#else
    (void)requirements;(void)options;(void)adapter;error="native profile loading requires Windows x64";return false;
#endif
}
} // namespace bedrock
