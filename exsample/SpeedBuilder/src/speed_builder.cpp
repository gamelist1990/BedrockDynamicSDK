#ifdef _WIN32
#include <bedrock/game_api.hpp>
#include <bedrock/game_hooks.hpp>
#include <bedrock/generated_symbols.hpp>
#include <bedrock/remote_catalog.hpp>
#include <bedrock/profile_manager.hpp>
#include "native.hpp"
#include "speed_builder/config.hpp"
#include <knownfolders.h>
#include <shlobj.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#if !defined(_MSC_VER) || _ITERATOR_DEBUG_LEVEL != 0
#error SpeedBuilder requires MSVC Release STL ABI.
#endif
namespace {
namespace sb=speed_builder;namespace native=speedbuilder_native;
using Clock=std::chrono::steady_clock;using Vec=bedrock::Vec3;
native::Bindings bindings;native::Reader reader(bindings);
bedrock::GameApi api;std::unique_ptr<bedrock::GameHooks> hooks;
std::atomic<bool> stopped{},gameplay{},cancelRequested{};
std::atomic<void*> client{};std::atomic<std::uint64_t> epoch{1};std::atomic<unsigned> status{};
std::atomic<std::shared_ptr<const sb::Config>> config{std::make_shared<const sb::Config>()};
std::atomic<unsigned> configRevision{};
std::mutex logMutex;std::deque<std::string> logs;std::atomic<unsigned> lostLogs{},lostTitles{},titleQueued{};
void note(std::string value){std::unique_lock lock(logMutex,std::try_to_lock);if(!lock||logs.size()>=128){++lostLogs;return;}value+=" at_ms="+std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());logs.emplace_back(std::move(value));}
struct TitleCopy{std::uint64_t epoch{};int type{};native::StringCopy text;};
std::mutex titleMutex;std::deque<TitleCopy> titles;
using TitleReadFn=void*(__fastcall*)(void*,void*,void*);TitleReadFn originalTitleRead{};BDS_HookToken titleToken{};
bool captureTitle(void* packet,void* result,TitleCopy* out)noexcept{
    std::uintptr_t vt{};unsigned char success{};
    return native::read(packet,0,vt)&&vt==bindings.titleVtable&&native::read(result,0x40,success)&&(success&1)&&
        native::read(packet,0x30,out->type)&&out->type>=0&&out->type<=8&&
        ((out->type==0||out->type==1||out->type==5)||native::copyString(static_cast<std::byte*>(packet)+0x38,&out->text));
}
void* __fastcall titleRead(void* packet,void* result,void* stream){
    const auto generation=epoch.load();void* value=originalTitleRead(packet,result,stream);
    if(stopped.load())return value;
    TitleCopy text;text.epoch=generation;
    if(captureTitle(packet,result,&text)&&(text.type==0||text.type==1||text.type==2||text.type==3||text.type==6||text.type==7)){
        std::unique_lock lock(titleMutex,std::try_to_lock);
        if(lock&&titles.size()<32){titles.push_back(text);++titleQueued;}else ++lostTitles;
    }return value;
}
std::string plain(std::string_view input){
    std::string out;out.reserve(input.size());
    for(std::size_t i=0;i<input.size();){const auto c=static_cast<unsigned char>(input[i]);
        if(c==0xc2&&i+2<input.size()&&static_cast<unsigned char>(input[i+1])==0xa7){i+=3;continue;}
        // 0xA7 can be a UTF-8 continuation byte in Japanese text.
        // Only the complete UTF-8 section sign above is a formatting marker.
        if(c>=32&&c!=127)out+=input[i];++i;
    }return out;
}
std::string titleText(const native::StringCopy& text,int type){
    auto value=plain(std::string_view(text.bytes,text.size));if(type<6)return value;
    try{auto j=nlohmann::json::parse(value);std::string result;
        const auto visit=[&](const auto& self,const nlohmann::json& node)->void{
            if(node.is_array())for(const auto& child:node)self(self,child);
            else if(node.is_object()){
                if(node.contains("text")&&node["text"].is_string())result+=node["text"].get<std::string>();
                if(node.contains("rawtext"))self(self,node["rawtext"]);
                if(node.contains("with"))self(self,node["with"]);
            }
        };visit(visit,j);return plain(result);
    }catch(...){return {};}
}
struct DiffJob{std::shared_ptr<const sb::PreviewSnapshot> before;sb::ObservationSnapshot after;std::uint64_t serial{};};
struct DiffResult{sb::PreparedCandidates candidates;std::uint64_t serial{};};
struct RemovalCheck{sb::PreparedCandidates candidates;std::uint64_t serial{},started{};std::size_t cursor{};};
std::optional<RemovalCheck> removalCheck;std::uint64_t rejectedResult{};
std::mutex diffMutex;std::shared_ptr<const DiffJob> diffInput;std::shared_ptr<const DiffResult> diffOutput;
std::unique_ptr<sb::Model> model;
std::shared_ptr<const sb::PreviewSnapshot> immutablePreview;
std::uint64_t generation{},runtime{},tick{},jobSequence{},capturePass{},lastQueuedPass{},acceptedResult{};
unsigned seenConfig{};void* playerIdentity{};void* sourceIdentity{};
std::string currentTitle,currentSubtitle;std::uint64_t subtitleTick{};
std::shared_ptr<const sb::Config> roundConfig;
struct PlacementPlan{sb::PlacementRequest request;Vec hit{};float yaw{},pitch{},score{};};
struct Probe{sb::PlacementRequest request;std::optional<std::uint64_t> targetState;unsigned candidate{};
    void* player{};std::uint64_t runtime{},world{},selectedTick{};int oldSlot=-1,heldSlot=-1;bool selectionReady{};
    unsigned noSupport{},outOfReach{},predictions{},nativeFailures{},mismatches{};int lastData=-1;
    float preferredYaw{},preferredPitch{};std::optional<PlacementPlan> fallback;
};
std::optional<Probe> probe;
struct CachedRecipe{sb::PlacementRecipe recipe;void* type{};int aux{};};
std::map<std::uint64_t,std::vector<CachedRecipe>> recipeCache;

struct Pending{sb::PlacementRequest request;std::uint64_t runtime{};void* player{};void* source{};std::uint64_t prepared{};
    float yaw{},pitch{},oldYaw{},oldPitch{};Vec hit{};int oldSlot=-1;bool dispatched{},rotationApplied{};};
std::optional<Pending> pending;
std::map<sb::BlockPos,std::pair<std::uint64_t,std::uint64_t>> mismatchLogs;
unsigned nativeCalls{},nativeAccepted{},placementDeferred{};std::string lastDeferred;
void restoreProbe(void* player,std::uint64_t liveRuntime){
    if(!probe)return;
    if(probe->player==player&&probe->runtime==liveRuntime&&probe->world==epoch.load()&&probe->oldSlot>=0&&
       bindings.selected(player)==probe->heldSlot)bindings.select(player,static_cast<unsigned>(probe->oldSlot));
    probe.reset();
}
std::uint64_t lastProgress{};sb::Phase lastPhase=sb::Phase::idle;
void restorePending(void* player,std::uint64_t liveRuntime){
    if(!pending)return;
    if(player==pending->player&&liveRuntime==pending->runtime&&generation==epoch.load()){
        auto* rot=bindings.component(player,bindings.actors.actorRotationComponentHash);float yaw{},pitch{};
        if(pending->rotationApplied&&native::read(rot,bindings.actors.rotationYawOffset,yaw)&&std::abs(yaw-pending->yaw)<.01f)
            native::write(rot,bindings.actors.rotationYawOffset,pending->oldYaw);
        if(pending->rotationApplied&&native::read(rot,bindings.actors.rotationPitchOffset,pitch)&&std::abs(pitch-pending->pitch)<.01f)
            native::write(rot,bindings.actors.rotationPitchOffset,pending->oldPitch);
        if(bindings.selected(player)==static_cast<int>(pending->request.hotbarSlot)&&pending->oldSlot>=0&&pending->oldSlot<9)
            bindings.select(player,static_cast<unsigned>(pending->oldSlot));
    }pending.reset();
}
void resetRound(std::uint64_t world){
    // Cross-world pointers are discarded; never restore into an expired world.
    pending.reset();probe.reset();removalCheck.reset();immutablePreview.reset();rejectedResult=0;reader.clear();roundConfig=config.load();model=std::make_unique<sb::Model>(*roundConfig);model->reset(world);
    currentTitle.clear();currentSubtitle.clear();lastQueuedPass=capturePass=acceptedResult=0;lastPhase=sb::Phase::idle;
    nativeCalls=nativeAccepted=placementDeferred=0;lastDeferred.clear();mismatchLogs.clear();recipeCache.clear();
    {std::unique_lock lock(diffMutex,std::try_to_lock);if(lock){diffInput.reset();diffOutput.reset();}}
}
void processTitles(const native::NativeHotbar& hotbar,const Vec& position){
    std::deque<TitleCopy> updates;{std::unique_lock lock(titleMutex,std::try_to_lock);if(lock){updates.swap(titles);titleQueued.fetch_sub(static_cast<unsigned>(updates.size()));}}
    for(const auto& update:updates){if(update.epoch!=generation)continue;
        if(update.type==0||update.type==1){currentTitle.clear();currentSubtitle.clear();}
        else if(update.type==2||update.type==6){currentTitle=titleText(update.text,update.type);if(tick-subtitleTick>10)currentSubtitle.clear();
            model->onPrompt(generation,tick,{},hotbar.copied,currentTitle,{});}
        else if(update.type==3||update.type==7){currentSubtitle=titleText(update.text,update.type);subtitleTick=tick;}
        if(update.type!=5)note("title received: type="+std::to_string(update.type)+" title="+currentTitle.substr(0,256)+" subtitle="+currentSubtitle.substr(0,256));
        if((model->phase()==sb::Phase::building||model->phase()==sb::Phase::capturing||model->phase()==sb::Phase::waiting)&&(currentSubtitle.find("時間切れ")!=std::string::npos||
           currentSubtitle.find("建築をスキャン")!=std::string::npos||currentSubtitle.find("結果を計算")!=std::string::npos||
           currentSubtitle.find("合格")!=std::string::npos)){
            restoreProbe(playerIdentity,runtime);restorePending(playerIdentity,runtime);model->cancel();note("round cancelled: server round ended");
        }
        const sb::BlockPos center{static_cast<int>(std::floor(position.x)),static_cast<int>(std::floor(position.y)),static_cast<int>(std::floor(position.z))};
        if(model->onPrompt(generation,tick,center,hotbar.copied,currentTitle,currentSubtitle)){
            restoreProbe(playerIdentity,runtime);restorePending(playerIdentity,runtime);removalCheck.reset();immutablePreview.reset();rejectedResult=0;lastQueuedPass=0;
            acceptedResult=0;mismatchLogs.clear();recipeCache.clear();
            {std::unique_lock lock(diffMutex,std::try_to_lock);if(lock){diffInput.reset();diffOutput.reset();}}
            note("memorizing: topic="+currentTitle+" subtitle="+currentSubtitle+" radius="+std::to_string(roundConfig->radius));
        }
    }
}
void capture(void* source){
    const auto beforePass=model->stats().capturePasses;
    const auto request=model->captureRequest(tick);if(request.positions.empty())return;
    const auto captureBudget=model->phase()==sb::Phase::capturing?2000:1000;
    const auto deadline=Clock::now()+std::chrono::microseconds(captureBudget);std::vector<sb::Cell> cells;cells.reserve(request.positions.size());
    for(const auto& pos:request.positions){auto cell=reader.getBlock(source,pos);
        if(cell.kind==sb::CellKind::unknown)note("capture unavailable at "+std::to_string(pos.x)+","+std::to_string(pos.y)+","+std::to_string(pos.z)+" [chunk/layout/native read]");
        cells.push_back(std::move(cell));if(Clock::now()>=deadline)break;}
    if(!model->submitCapture(request.token,tick,cells))return;
    const auto stats=model->stats();capturePass=stats.capturePasses;
    if(capturePass==1&&beforePass==0)note("capture ready: initial_cells="+std::to_string(stats.captured)+" observation_cells="+std::to_string(model->observationCellCount())+
        " cap_per_tick="+std::to_string(roundConfig->captureCellsPerTick)+" cooperative_budget_us=2000");
    if(model->phase()!=sb::Phase::waiting||!model->previewFrozen()||capturePass==beforePass||capturePass==lastQueuedPass)return;
    if(!immutablePreview||immutablePreview->token!=model->token()){
        auto before=model->previewSnapshot();if(!before)return;
        immutablePreview=std::make_shared<const sb::PreviewSnapshot>(std::move(*before));
    }
    auto after=model->observationSnapshot(true);if(!after)return;
    // Copy the full baseline once per round; subsequent worker jobs share it
    // and transfer only the original non-air observations.
    auto job=std::make_shared<DiffJob>(DiffJob{immutablePreview,std::move(*after),++jobSequence});
    std::unique_lock lock(diffMutex,std::try_to_lock);if(lock){diffInput=std::move(job);lastQueuedPass=capturePass;}
}
std::string propertyText(const sb::Cell& cell){
    nlohmann::json out=nlohmann::json::object();
    out["legacy_data"]=cell.legacyData?nlohmann::json(*cell.legacyData):nlohmann::json(nullptr);
    out["state_identity"]=cell.stateIdentity.value_or(0);
    if(!cell.properties)out["properties"]=nullptr;
    else {auto values=nlohmann::json::object();for(const auto& [key,value]:cell.properties->values)std::visit([&](const auto& v){values[key]=v;},value);out["properties"]=std::move(values);}
    return out.dump();
}
bool sameBlockType(const native::BlockInfo* expected,const native::BlockInfo* actual){
    return expected&&actual&&(expected->type==actual->type||expected->name==actual->name);
}
bool neighborDerived(std::string_view id){
    return id.ends_with("_fence")||id.ends_with("_wall")||id.ends_with("_pane")||id=="minecraft:iron_bars";
}
bool probeNext(void* player,void* source,const native::NativeHotbar& hotbar,Vec eye){
    if(!probe)return false;auto& p=*probe;
    if(!model->refreshPlacement(p.request.token,p.request.actionId,tick)){restoreProbe(player,runtime);return false;}
    const auto slot=p.request.hotbarSlot;const auto target=p.request.target;const auto* goal=model->recordedCell(target);
    const auto now=reader.getBlock(source,target);
    if(!goal||(p.request.recipe.augment?!sb::canAugment(*goal,now):now.kind!=sb::CellKind::air)){
        model->deferPlacement(p.request.token,p.request.actionId,tick);restoreProbe(player,runtime);return false;
    }
    auto* wanted=reader.describe(reinterpret_cast<void*>(p.targetState.value_or(0)));
    const auto heldName=slot<9?reader.typeName(hotbar.types[slot]):std::string{};
    if(!wanted||slot>=9||!hotbar.types[slot]||!hotbar.copied[slot].count||
       hotbar.copied[slot].identifier!=p.request.recipe.itemIdentifier||heldName!=p.request.recipe.itemIdentifier){
        ++placementDeferred;lastDeferred="probe_item_or_type_changed";model->deferPlacement(p.request.token,p.request.actionId,tick);restoreProbe(player,runtime);return false;
    }
    if(p.oldSlot<0)p.oldSlot=bindings.selected(player);
    if(p.oldSlot<0||p.oldSlot>8){model->deferPlacement(p.request.token,p.request.actionId,tick);restoreProbe(player,runtime);return false;}
    if(!p.selectionReady){
        if(!bindings.select(player,slot)){++placementDeferred;lastDeferred="select_slot_failed";model->deferPlacement(p.request.token,p.request.actionId,tick);restoreProbe(player,runtime);return false;}
        p.heldSlot=static_cast<int>(slot);p.selectionReady=true;p.selectedTick=tick;return false;
    }
    if(bindings.selected(player)!=static_cast<int>(slot)){++placementDeferred;lastDeferred="selected_slot_changed";model->deferPlacement(p.request.token,p.request.actionId,tick);restoreProbe(player,runtime);return false;}
    if(tick<=p.selectedTick)return false;
    const auto commit=[&](const PlacementPlan& plan){
        Pending action;action.request=plan.request;action.runtime=runtime;action.player=player;action.source=source;
        action.prepared=tick;action.yaw=plan.yaw;action.pitch=plan.pitch;action.hit=plan.hit;action.oldSlot=p.oldSlot;
        // A fallback may have come from another matching hotbar slot. Select it
        // again and retain the normal one-tick main-hand boundary before use.
        if(!bindings.select(player,action.request.hotbarSlot))return false;
        p.heldSlot=static_cast<int>(action.request.hotbarSlot);
        auto* rot=bindings.component(player,bindings.actors.actorRotationComponentHash);
        if(!native::read(rot,bindings.actors.rotationYawOffset,action.oldYaw)||!native::read(rot,bindings.actors.rotationPitchOffset,action.oldPitch))return false;
        if(!native::write(rot,bindings.actors.rotationYawOffset,action.yaw)||!native::write(rot,bindings.actors.rotationPitchOffset,action.pitch)){
            native::write(rot,bindings.actors.rotationYawOffset,action.oldYaw);native::write(rot,bindings.actors.rotationPitchOffset,action.oldPitch);return false;
        }
        if(plan.request.recipe.exactStateSupported){
            if(recipeCache.size()<2048){auto& choices=recipeCache[p.targetState.value_or(0)];
                if(choices.size()<6&&std::none_of(choices.begin(),choices.end(),[&](const CachedRecipe& c){return c.recipe==plan.request.recipe&&c.type==hotbar.types[plan.request.hotbarSlot]&&c.aux==hotbar.aux[plan.request.hotbarSlot];}))
                    choices.push_back({plan.request.recipe,hotbar.types[plan.request.hotbarSlot],hotbar.aux[plan.request.hotbarSlot]});}
        }
        action.rotationApplied=true;pending=std::move(action);probe.reset();return true;
    };
    if(p.request.recipe.augment){
        PlacementPlan plan;plan.request=p.request;plan.request.support=target;plan.request.recipe.face=sb::Face::up;
        plan.request.recipe.hitX=plan.request.recipe.hitZ=.5f;plan.request.recipe.hitY=1.f;
        plan.request.recipe.deferredState=true;plan.request.recipe.exactStateSupported=false;
        plan.yaw=p.preferredYaw;plan.pitch=p.preferredPitch;plan.hit={target.x+.5f,target.y+1.f,target.z+.5f};
        return commit(plan);
    }
    if(p.candidate==0)if(auto found=recipeCache.find(p.targetState.value_or(0));found!=recipeCache.end()){
        for(const auto& c:found->second){if(c.type!=hotbar.types[slot]||c.aux!=hotbar.aux[slot])continue;
            const auto n=sb::faceNormal(c.recipe.face);sb::BlockPos support{target.x-n.x,target.y-n.y,target.z-n.z};
            const auto base=reader.getBlock(source,support,true);if(base.kind!=sb::CellKind::block||(!base.supportsPlacement&&!goal->nativePlacementCandidate))continue;
            Vec hit{support.x+c.recipe.hitX,support.y+c.recipe.hitY,support.z+c.recipe.hitZ};
            const double dx=double(hit.x)-eye.x,dy=double(hit.y)-eye.y,dz=double(hit.z)-eye.z;
            if(dx*dx+dy*dy+dz*dz>roundConfig->maxReach*roundConfig->maxReach)continue;
            void* predicted{};
            if(bindings.predict(hotbar.types[slot],player,target,static_cast<unsigned>(c.recipe.face),{c.recipe.hitX,c.recipe.hitY,c.recipe.hitZ},hotbar.aux[slot],c.recipe.yaw,c.recipe.pitch,&predicted)&&
               reinterpret_cast<std::uintptr_t>(predicted)==p.targetState){PlacementPlan plan;plan.request=p.request;plan.request.support=support;plan.request.recipe=c.recipe;
                plan.yaw=c.recipe.yaw;plan.pitch=c.recipe.pitch;plan.hit=hit;return commit(plan);}
        }
    }
    constexpr unsigned faces[6]={1,2,3,4,5,0};constexpr float heights[3]={.5f,.25f,.75f};constexpr float pitches[3]={0.f,90.f,-90.f};
    const auto deadline=Clock::now()+std::chrono::microseconds(1500);
    for(unsigned step=0;step<64&&p.candidate<216&&Clock::now()<deadline;++step){const unsigned n=p.candidate++;unsigned i=n;
        const auto face=faces[i%6];i/=6;const unsigned turn=i%4;const float yaw=p.preferredYaw+float(turn)*90.f;i/=4;
        const float height=heights[i%3];i/=3;const unsigned tilt=i%3;const float pitch=tilt==0?p.preferredPitch:pitches[tilt];
        const auto normal=sb::faceNormal(static_cast<sb::Face>(face));sb::BlockPos support{target.x-normal.x,target.y-normal.y,target.z-normal.z};
        const auto base=reader.getBlock(source,support,true);if(base.kind!=sb::CellKind::block||(!base.supportsPlacement&&!goal->nativePlacementCandidate)){++p.noSupport;continue;}
        Vec fractional{.5f,height,.5f};if(face==0)fractional.y=0;else if(face==1)fractional.y=1;
        else if(face==2)fractional.z=0;else if(face==3)fractional.z=1;else if(face==4)fractional.x=0;else fractional.x=1;
        Vec hit{support.x+fractional.x,support.y+fractional.y,support.z+fractional.z};
        const double dx=double(hit.x)-eye.x,dy=double(hit.y)-eye.y,dz=double(hit.z)-eye.z;
        if(dx*dx+dy*dy+dz*dz>roundConfig->maxReach*roundConfig->maxReach){++p.outOfReach;continue;}
        void* predicted{};++p.predictions;
        const bool ok=bindings.predict(hotbar.types[slot],player,target,face,fractional,hotbar.aux[slot],yaw,pitch,&predicted);
        if(!ok){++p.nativeFailures;continue;}
        auto* predictedInfo=reader.describe(predicted);if(!sameBlockType(wanted,predictedInfo))continue;
        const auto predictedCell=reader.describeCell(predicted,target);
        const bool exact=reinterpret_cast<std::uintptr_t>(predicted)==p.targetState;
        if(!exact){++p.mismatches;std::uint16_t data{};if(native::read(predicted,bindings.layout("block_state_data_offset"),data))p.lastData=data;}
        PlacementPlan plan;plan.request=p.request;plan.request.support=support;auto& recipe=plan.request.recipe;
        recipe.face=static_cast<sb::Face>(face);recipe.orientationKnown=true;recipe.exactStateSupported=exact;recipe.deferredState=!exact;
        recipe.yaw=yaw;recipe.pitch=pitch;recipe.hitX=fractional.x;recipe.hitY=fractional.y;recipe.hitZ=fractional.z;
        plan.yaw=yaw;plan.pitch=pitch;plan.hit=hit;plan.score=-float(turn)-float(tilt)*.1f;
        if(goal->properties&&predictedCell.properties)for(const auto& [key,value]:goal->properties->values){
            auto it=predictedCell.properties->values.find(key);if(it!=predictedCell.properties->values.end()&&value==it->second)plan.score+=sb::derivedState(key)?1.f:16.f;
        }
        if(exact||sb::samePlacementBasis(*goal,predictedCell))return commit(plan);
        // Type-only proposals cannot establish slab half, stair facing, barrel
        // facing or pillar axis. Only connection-derived blocks may settle later.
        if(neighborDerived(goal->identifier)&&(!p.fallback||plan.score>p.fallback->score))p.fallback=std::move(plan);
        if(Clock::now()>=deadline)break;
    }
    if(p.candidate>=216){
        // Explore every matching slot before selecting the best type-compatible
        // proposal. Server-derived permutations must be checked after placement.
        for(unsigned next=slot+1;next<9;++next)if(hotbar.copied[next].count&&hotbar.copied[next].identifier==p.request.recipe.itemIdentifier){
            p.request.hotbarSlot=next;p.candidate=0;p.selectionReady=false;return false;
        }
        if(p.fallback){auto plan=*p.fallback;note("placement deferred-state proposal: item="+plan.request.recipe.itemIdentifier+" slot="+std::to_string(plan.request.hotbarSlot)+" expected="+propertyText(*goal));return commit(plan);}
        note("placement unresolved: "+p.request.recipe.itemIdentifier+" no_support="+std::to_string(p.noSupport)+" reach_rejected="+std::to_string(p.outOfReach)+
            " predictions="+std::to_string(p.predictions)+" native_failures="+std::to_string(p.nativeFailures)+" state_mismatch="+std::to_string(p.mismatches)+" expected="+propertyText(*goal));
        ++placementDeferred;lastDeferred="await_exact_state_or_support";
        model->deferPlacement(p.request.token,p.request.actionId,tick,20);restoreProbe(player,runtime);
    }return false;
}
void tickGame(const bedrock::LevelTickEvent& event){
    void* instance=client.load();void* player=bindings.local(instance);void* level{};
    if(!player||!native::read(player,bindings.layout("player_level_offset"),level)||level!=event.level)return;
    Vec position{};std::uint64_t id{};if(!bindings.pose(player,position,id))return;
    auto* source=bindings.region(instance);if(!source)return;
    const auto world=epoch.load();
    if(!model||generation!=world||playerIdentity!=player||runtime!=id||sourceIdentity!=source){
        const auto reasons=std::string(generation!=world?"epoch ":"")+(playerIdentity!=player?"player ":"")+(runtime!=id?"runtime_id ":"")+(sourceIdentity!=source?"region ":"");
        generation=world;playerIdentity=player;runtime=id;sourceIdentity=source;seenConfig=configRevision.load();resetRound(world);note("world identity ready; recording/build caches reset; changed="+reasons);
    }
    ++tick;
    if(seenConfig!=configRevision.load()){restoreProbe(player,id);restorePending(player,id);seenConfig=configRevision.load();resetRound(world);note("config applied; round cancelled");}
    model->update(generation,tick);
    if(stopped.load()||cancelRequested.exchange(false)||!gameplay.load()||!roundConfig->enabled){restoreProbe(player,id);restorePending(player,id);if(model->phase()!=sb::Phase::idle&&model->phase()!=sb::Phase::cancelled)model->cancel();return;}
    if((model->phase()==sb::Phase::idle||model->phase()==sb::Phase::complete||model->phase()==sb::Phase::cancelled||model->phase()==sb::Phase::failed)&&!titleQueued.load()){restoreProbe(player,id);restorePending(player,id);return;}
    native::NativeHotbar hotbar;if(!reader.hotbar(player,hotbar)){restoreProbe(player,id);restorePending(player,id);model->cancel();note("round cancelled: hotbar read failed");return;}
    processTitles(hotbar,position);model->observeHotbar(hotbar.copied);
    if(removalCheck&&removalCheck->candidates.token!=model->token())removalCheck.reset();
    if(model->phase()==sb::Phase::capturing||(model->phase()==sb::Phase::waiting&&!removalCheck))capture(source);
    if(model->phase()==sb::Phase::waiting){std::shared_ptr<const DiffResult> ready;
        {std::unique_lock lock(diffMutex,std::try_to_lock);if(lock)ready=diffOutput;}
        if(!removalCheck&&ready&&ready->serial!=rejectedResult&&ready->serial!=acceptedResult&&
           ready->candidates.token==model->token()&&ready->candidates.status==sb::DiffStatus::ready){
            bool matchingItem=false;
            for(const auto& c:ready->candidates.removed)if(c.placement)for(const auto& slot:hotbar.copied)
                matchingItem|=slot.count&&slot.identifier==c.placement->itemIdentifier;
            if(matchingItem)removalCheck=RemovalCheck{ready->candidates,ready->serial,tick,0};
        }
        if(removalCheck&&tick>removalCheck->started){
            const auto deadline=Clock::now()+std::chrono::microseconds(1500);unsigned checked=0;bool invalid=false;
            auto& check=*removalCheck;
            while(check.cursor<check.candidates.removed.size()&&checked++<128){
                const auto cell=reader.getBlock(source,check.candidates.removed[check.cursor].position);
                if(cell.kind==sb::CellKind::unknown)break;
                if(cell.kind!=sb::CellKind::air){invalid=true;break;}
                ++check.cursor;if(Clock::now()>=deadline)break;
            }
            if(invalid){rejectedResult=check.serial;removalCheck.reset();note("removal confirmation changed; resume sparse observation");}
            else if(check.cursor==check.candidates.removed.size()&&model->acceptCandidates(check.candidates,tick,hotbar.copied)){
                acceptedResult=check.serial;removalCheck.reset();
                note("building: removed preview blocks="+std::to_string(model->stats().queued)+" permanent scenery excluded="+std::to_string(model->stats().persistent)+" [target-only second observation]");
                for(unsigned slot=0;slot<9;++slot)if(hotbar.copied[slot].count)note("build hotbar: slot="+std::to_string(slot)+" item="+hotbar.copied[slot].identifier+" count="+std::to_string(hotbar.copied[slot].count)+" effective_aux="+std::to_string(hotbar.aux[slot]));
            }
        }
    }
    Vec eye=position; // Native Player origin; reach/ray semantics still require live confirmation.
    if(pending){
        if(pending->dispatched){restorePending(player,id);}
        else if(tick<=pending->prepared){
            // Retain the selected main hand and pose through preparation; dispatch
            // on the next client tick, rechecking the exact prediction first.
            auto* rot=bindings.component(player,bindings.actors.actorRotationComponentHash);
            if(!native::write(rot,bindings.actors.rotationYawOffset,pending->yaw)||!native::write(rot,bindings.actors.rotationPitchOffset,pending->pitch)){
                model->deferPlacement(pending->request.token,pending->request.actionId,tick);restorePending(player,id);
            }
        }
        else {
            auto& action=*pending;const auto token=action.request.token;const auto aid=action.request.actionId;
            float yaw{},pitch{};auto* rot=bindings.component(player,bindings.actors.actorRotationComponentHash);
            const auto target=reader.getBlock(source,action.request.target);const auto support=reader.getBlock(source,action.request.support,true);void* predicted{};
            const auto desired=model->recordedState(action.request.target).value_or(0);
            const auto& recipe=action.request.recipe;const unsigned slot=action.request.hotbarSlot;
            const double dx=double(action.hit.x)-eye.x,dy=double(action.hit.y)-eye.y,dz=double(action.hit.z)-eye.z;
            const char* reason=nullptr;
            if(generation!=epoch.load()||!model->actionPending(token,aid)||action.runtime!=id||action.source!=source)reason="expired_action";
            else if(slot>=9||bindings.selected(player)!=static_cast<int>(slot)||!hotbar.copied[slot].count||hotbar.copied[slot].identifier!=recipe.itemIdentifier||!hotbar.types[slot]||reader.typeName(hotbar.types[slot])!=recipe.itemIdentifier)reason="main_hand_changed";
            else if(recipe.augment?(!model->recordedCell(action.request.target)||!sb::canAugment(*model->recordedCell(action.request.target),target)):target.kind!=sb::CellKind::air)reason="target_changed";
            else if(!recipe.augment&&(support.kind!=sb::CellKind::block||
                    (!support.supportsPlacement&&(!model->recordedCell(action.request.target)||!model->recordedCell(action.request.target)->nativePlacementCandidate))))reason="no_support";
            else if(dx*dx+dy*dy+dz*dz>roundConfig->maxReach*roundConfig->maxReach)reason="out_of_reach";
            else if(!native::read(rot,bindings.actors.rotationYawOffset,yaw)||!native::read(rot,bindings.actors.rotationPitchOffset,pitch))reason="rotation_read_failed";
            if(!reason){
                if(!action.rotationApplied){action.oldYaw=yaw;action.oldPitch=pitch;action.rotationApplied=true;}
                if(!native::write(rot,bindings.actors.rotationYawOffset,action.yaw)||!native::write(rot,bindings.actors.rotationPitchOffset,action.pitch))reason="rotation_write_failed";
                else if(!recipe.augment){
                    if(!bindings.predict(hotbar.types[slot],player,action.request.target,static_cast<unsigned>(recipe.face),{recipe.hitX,recipe.hitY,recipe.hitZ},hotbar.aux[slot],action.yaw,action.pitch,&predicted))reason="prediction_failed";
                    else if(!recipe.deferredState&&reinterpret_cast<std::uintptr_t>(predicted)!=desired)reason="prediction_state_changed";
                    else if(recipe.deferredState){auto* wanted=reader.describe(reinterpret_cast<void*>(desired));auto* got=reader.describe(predicted);
                        if(!sameBlockType(wanted,got))reason="prediction_type_changed";
                        else if(!neighborDerived(recipe.itemIdentifier)&&(!model->recordedCell(action.request.target)||!sb::samePlacementBasis(*model->recordedCell(action.request.target),reader.describeCell(predicted,action.request.target))))reason="prediction_basis_changed";}
                }
            }
            if(!reason){
                bool accepted=false;auto* gm=bindings.gameMode(player);
                const bool dispatched=gm&&bindings.build(player,gm,action.request.support,static_cast<unsigned>(recipe.face),eye,action.hit,&accepted);
                if(dispatched)++nativeCalls;if(dispatched&&accepted)++nativeAccepted;
                model->placementResult(token,aid,dispatched&&accepted,tick);action.dispatched=true;
                note(std::string("placement native: called=")+(dispatched?"1":"0")+" accepted="+(accepted?"1":"0")+" game_mode="+(gm?"valid":"unavailable")+
                    " yaw="+std::to_string(action.yaw)+" pitch="+std::to_string(action.pitch)+" face="+std::to_string(static_cast<unsigned>(recipe.face))+
                    " support="+std::to_string(action.request.support.x)+","+std::to_string(action.request.support.y)+","+std::to_string(action.request.support.z)+
                    " deferred_state="+std::to_string(recipe.deferredState)+" augment="+std::to_string(recipe.augment)+" item="+recipe.itemIdentifier+" slot="+std::to_string(slot)+" target="+std::to_string(action.request.target.x)+","+std::to_string(action.request.target.y)+","+std::to_string(action.request.target.z));
            }else {++placementDeferred;lastDeferred=reason;model->deferPlacement(token,aid,tick,10);restorePending(player,id);}

        }
    }
    if(model->phase()==sb::Phase::building&&!pending){
        if(!probe){sb::BuildFrame frame;frame.world=generation;frame.tick=tick;frame.eye={eye.x,eye.y,eye.z};frame.hotbar=hotbar.copied;
            for(const auto& pos:model->inspectionPositions()){
                auto observed=reader.getBlock(source,pos,true);
                if(const auto* goal=model->recordedCell(pos);goal&&sb::sameType(*goal,observed)&&!sb::sameState(*goal,observed)){
                    auto& last=mismatchLogs[pos];const auto state=observed.stateIdentity.value_or(0);
                    if(last.first!=state||tick-last.second>=100){last={state,tick};
                        note("state mismatch: item="+goal->identifier+" target="+std::to_string(pos.x)+","+std::to_string(pos.y)+","+std::to_string(pos.z)+
                            " expected="+propertyText(*goal)+" actual="+propertyText(observed));}
                }
                frame.cells.push_back(std::move(observed));
            }
            if(auto request=model->nextPlacement(frame)){
                const auto desired=model->recordedState(request->target);
                if(desired){probe=Probe{*request,desired,0};probe->player=player;probe->runtime=id;probe->world=generation;
                    // Canonical quadrant centres avoid boundary-dependent results
                    // from the user's current arbitrary view angle.
                    probe->preferredYaw=0.f;probe->preferredPitch=0.f;
                    if(const auto* goal=model->recordedCell(request->target))if(auto angles=sb::placementAngles(*goal)){probe->preferredYaw=angles->first;probe->preferredPitch=angles->second;}
                }else model->deferPlacement(request->token,request->actionId,tick);
            }
        }
        if(probe)probeNext(player,source,hotbar,eye);
    }
    const auto phase=model->phase();
    if(phase!=lastPhase||tick-lastProgress>=100){lastPhase=phase;lastProgress=tick;
        if(phase!=sb::Phase::idle&&phase!=sb::Phase::cancelled){const auto stats=model->stats();note("progress: phase="+std::to_string(static_cast<int>(phase))+" captured="+std::to_string(stats.captured)+" observed="+std::to_string(stats.confirmed)+" remaining="+std::to_string(stats.remaining)+" provisional="+std::to_string(stats.provisional)+" exhausted="+std::to_string(stats.exhausted)+" stop_reason="+std::to_string(static_cast<int>(model->stopReason())));}}
    if(phase==sb::Phase::waiting&&tick%100==0){
        std::shared_ptr<const DiffResult> ready;{std::unique_lock lock(diffMutex,std::try_to_lock);if(lock)ready=diffOutput;}
        const auto reason=!model->previewFrozen()?"preview_not_removed":!ready?"diff_pending":
            ready->candidates.token!=model->token()?"stale_diff":removalCheck&&removalCheck->cursor<removalCheck->candidates.removed.size()?"target_confirmation":
            ready->candidates.status!=sb::DiffStatus::ready?"diff_not_ready":"await_matching_hotbar_item";
        note(std::string("build wait: reason=")+reason+" passes="+std::to_string(model->stats().capturePasses)+
            " frozen="+std::to_string(model->previewFrozen())+" diff="+(ready?std::to_string(static_cast<int>(ready->candidates.status)):"none")+
            " confirmation="+std::to_string(removalCheck?removalCheck->cursor:0)+" removed="+std::to_string(ready?ready->candidates.removed.size():0)+" changed_excluded="+std::to_string(ready?ready->candidates.changedExcluded:0));
        for(unsigned slot=0;slot<9;++slot)if(hotbar.copied[slot].count)note("waiting hotbar: slot="+std::to_string(slot)+" item="+hotbar.copied[slot].identifier+" count="+std::to_string(hotbar.copied[slot].count));
    }
    if(phase==sb::Phase::building&&tick%100==0){
        const auto scan=model->placementScan();
        note("placement pipeline: visited="+std::to_string(scan.visited)+" unknown="+std::to_string(scan.unknown)+" occupied="+std::to_string(scan.occupied)+
            " no_support="+std::to_string(scan.noSupport)+" dependency="+std::to_string(scan.dependency)+" out_of_reach="+std::to_string(scan.outOfReach)+
            " state_pending="+std::to_string(scan.statePending)+" missing_item="+std::to_string(scan.missingItem)+" inflight="+std::to_string(scan.inFlight)+" cooldown="+std::to_string(scan.cooldown)+
            " probe="+std::to_string(probe?probe->candidate:0)+" pending="+std::to_string(bool(pending))+" native_calls="+std::to_string(nativeCalls)+
            " accepted="+std::to_string(nativeAccepted)+" deferred="+std::to_string(placementDeferred)+" last_defer="+lastDeferred);
    }
    if(phase==sb::Phase::complete||phase==sb::Phase::failed||phase==sb::Phase::cancelled){restoreProbe(player,id);restorePending(player,id);}
}
void onTick(const bedrock::LevelTickEvent& event){try{tickGame(event);}catch(const std::exception& e){cancelRequested.store(true);note(std::string("tick failed: ")+e.what());}}
std::filesystem::path directory(){PWSTR path{};if(FAILED(SHGetKnownFolderPath(FOLDERID_Documents,KF_FLAG_CREATE,nullptr,&path)))throw std::runtime_error("Documents directory unavailable");
    auto dir=std::filesystem::path(path)/L"PEXData"/L"SpeedBuilder";CoTaskMemFree(path);return dir;}
void worker(const std::filesystem::path& dir){
    std::ofstream file(dir/L"SpeedBuilder.log",std::ios::trunc);file<<"SpeedBuilder 1.26.52.3 candidate revision6; native reads/writes on client tick; copied diff worker\n";
    const auto path=dir/L"config.json";auto modified=std::filesystem::last_write_time(path);auto nextReload=Clock::now();
    while(!stopped.load()){
        std::shared_ptr<const DiffJob> job;{std::lock_guard lock(diffMutex);job=std::move(diffInput);diffInput.reset();}
        if(job&&job->before->token.world==epoch.load()){
            auto result=std::make_shared<DiffResult>(DiffResult{sb::prepareCandidates(*job->before,job->after),job->serial});
            if(result->candidates.token.world==epoch.load()){std::lock_guard lock(diffMutex);diffOutput=std::move(result);}
        }
        std::deque<std::string> batch;{std::lock_guard lock(logMutex);batch.swap(logs);}
        for(const auto& line:batch)file<<line<<'\n';if(!batch.empty())file.flush();
        if(auto lost=lostTitles.exchange(0))file<<"title mailbox dropped="<<lost<<"; cancel current round if title was missed\n";
        if(auto lost=lostLogs.exchange(0))file<<"log queue dropped="<<lost<<'\n';
        if(Clock::now()>=nextReload){nextReload=Clock::now()+std::chrono::milliseconds(500);std::error_code ec;const auto current=std::filesystem::last_write_time(path,ec);
            if(!ec&&current!=modified){modified=current;try{std::ifstream in(path);std::string text((std::istreambuf_iterator<char>(in)),{});auto next=std::make_shared<const sb::Config>(sb::parseConfig(text));if(sb::configJson(*next)!=sb::configJson(*config.load())){config.store(std::move(next));++configRevision;file<<"config changed; apply on client tick\n";}}catch(const std::exception& e){file<<"config rejected: "<<e.what()<<'\n';}}}
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
DWORD WINAPI start(void*){
    HMODULE pinned{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&start),&pinned);status.store(1);
    try{
        const auto dir=directory();std::filesystem::create_directories(dir);const auto path=dir/L"config.json";
        if(!std::filesystem::exists(path)){std::ofstream out(path);out<<sb::configJson(sb::Config{}).dump(2)<<'\n';}
        {std::ifstream in(path);std::string text((std::istreambuf_iterator<char>(in)),{});in.close();auto parsed=std::make_shared<const sb::Config>(sb::parseConfig(text));config.store(parsed);
            auto json=nlohmann::json::parse(text);if(json.value("config_revision",1)<2){json["config_revision"]=2;json["capture_cells_per_tick"]=parsed->captureCellsPerTick;json["build_interval_ticks"]=parsed->buildIntervalTicks;
                std::ofstream out(path,std::ios::trunc);out<<json.dump(2)<<'\n';if(!out)throw std::runtime_error("config migration failed");note("config upgraded: previous default capture cap256 ->1024 and interval2 ->1; non-default values preserved");}}
        std::string error;bedrock::ProfileManager profiles;
        const bedrock::ProfileRequirements required{bedrock::Feature::World,bedrock::Feature::TitlePackets,bedrock::Feature::WorldEvents};
        const bedrock::ProfileOptions options{bedrock::UpdatePolicy::RefreshIfNeeded,bedrock::CandidatePolicy::AllowCandidates};
        if(!profiles.prepare(required,options,[](const bedrock::RuntimeProfile& selected,std::string& failure){
            try{bindings.load(selected.json());return true;}catch(const std::exception& e){failure=e.what();return false;}
        },error))throw std::runtime_error(error);
        const auto selected=profiles.snapshot();const auto& profile=selected->json();const auto& resolver=selected->resolver();
        reader.states.reserve(4096);reader.names.reserve(512);
        if(!BDS_HookDirectInstall(bindings.titleReadFn,reinterpret_cast<void*>(&titleRead),reinterpret_cast<void**>(&originalTitleRead),&titleToken))throw std::runtime_error("SetTitlePacket read hook conflict");
        api.Event.Enum.Gameplay.then([](const bedrock::GameplayEvent& event){client.store(event.clientInstance);gameplay.store(event.active);if(!event.active)cancelRequested.store(true);});
        api.Event.Enum.WorldChange.then([](const bedrock::WorldChangeEvent& event){if(event.clientInstance)client.store(event.clientInstance);++epoch;cancelRequested.store(true);});
        api.Event.Enum.Keyboard.then([](const bedrock::KeyEvent& event){if(event.pressed&&event.inputKey()==config.load()->cancelKey)cancelRequested.store(true);});
        api.Event.Enum.LevelTick.then(onTick);hooks=std::make_unique<bedrock::GameHooks>(api);
        if(!hooks->installInput(resolver,error)||!hooks->installGameplay(resolver,error)||!hooks->installLevelTick(resolver,error)||!hooks->installWorldEvents(resolver,profile,error))throw std::runtime_error(error);
        note("ready; press Esc and return to gameplay; title/subtitle trigger enabled; cancel key="+std::to_string(config.load()->cancelKey));note(BDS_HookRuntimeDiagnostic());status.store(2);worker(dir);
    }catch(const std::exception& e){status.store(4);try{const auto dir=directory();std::filesystem::create_directories(dir);std::ofstream out(dir/L"SpeedBuilder.log",std::ios::app);out<<"startup failed: "<<e.what()<<'\n';}catch(...){} }
    stopped.store(true);cancelRequested.store(true);if(status.load()!=4)status.store(3);return 0;
}
}
extern "C" __declspec(dllexport) unsigned SpeedBuilder_GetStatus(){return status.load();}
extern "C" __declspec(dllexport) void SpeedBuilder_Stop(){stopped.store(true);cancelRequested.store(true);}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){DisableThreadLibraryCalls(instance);auto thread=CreateThread(nullptr,0,start,nullptr,0,nullptr);if(thread)CloseHandle(thread);else status.store(4);}else if(reason==DLL_PROCESS_DETACH)stopped.store(true);return TRUE;}
#endif
