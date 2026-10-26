#ifdef _WIN32
#include <bedrock/game_api.hpp>
#include <bedrock/game_hooks.hpp>
#include <bedrock/generated_symbols.hpp>
#include <bedrock/remote_catalog.hpp>
#include "native.hpp"
#include "speed_builder/config.hpp"
#include <knownfolders.h>
#include <shlobj.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
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
void note(std::string value){std::unique_lock lock(logMutex,std::try_to_lock);if(!lock||logs.size()>=128){++lostLogs;return;}logs.emplace_back(std::move(value));}
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
    if(captureTitle(packet,result,&text)){
        std::unique_lock lock(titleMutex,std::try_to_lock);
        if(lock&&titles.size()<32){titles.push_back(text);++titleQueued;}else ++lostTitles;
    }return value;
}
std::string plain(std::string_view input){
    std::string out;out.reserve(input.size());
    for(std::size_t i=0;i<input.size();){const auto c=static_cast<unsigned char>(input[i]);
        if(c==0xc2&&i+2<input.size()&&static_cast<unsigned char>(input[i+1])==0xa7){i+=3;continue;}
        if(c==0xa7&&i+1<input.size()){i+=2;continue;}
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
struct DiffJob{sb::PreviewSnapshot before;sb::ObservationSnapshot after;std::uint64_t serial{};};
struct DiffResult{sb::PreparedCandidates candidates;std::uint64_t serial{};};
std::mutex diffMutex;std::shared_ptr<const DiffJob> diffInput;std::shared_ptr<const DiffResult> diffOutput;
std::unique_ptr<sb::Model> model;
std::uint64_t generation{},runtime{},tick{},jobSequence{},capturePass{},lastQueuedPass{},acceptedResult{};
unsigned seenConfig{};void* playerIdentity{};void* sourceIdentity{};
std::string currentTitle,currentSubtitle;std::uint64_t subtitleTick{};
std::shared_ptr<const sb::Config> roundConfig;
struct Probe{sb::PlacementRequest request;std::optional<std::uint64_t> targetState;unsigned candidate{};};
std::optional<Probe> probe;
struct Pending{sb::PlacementRequest request;std::uint64_t runtime{};void* player{};void* source{};std::uint64_t prepared{};
    float yaw{},pitch{},oldYaw{},oldPitch{};Vec hit{};int oldSlot=-1;bool dispatched{};};
std::optional<Pending> pending;
std::uint64_t lastProgress{};sb::Phase lastPhase=sb::Phase::idle;
void restorePending(void* player,std::uint64_t liveRuntime){
    if(!pending)return;
    if(player==pending->player&&liveRuntime==pending->runtime&&generation==epoch.load()){
        auto* rot=bindings.component(player,bindings.actors.actorRotationComponentHash);float yaw{},pitch{};
        if(native::read(rot,bindings.actors.rotationYawOffset,yaw)&&std::abs(yaw-pending->yaw)<.01f)
            native::write(rot,bindings.actors.rotationYawOffset,pending->oldYaw);
        if(native::read(rot,bindings.actors.rotationPitchOffset,pitch)&&std::abs(pitch-pending->pitch)<.01f)
            native::write(rot,bindings.actors.rotationPitchOffset,pending->oldPitch);
        if(bindings.selected(player)==static_cast<int>(pending->request.hotbarSlot)&&pending->oldSlot>=0&&pending->oldSlot<9)
            bindings.select(player,static_cast<unsigned>(pending->oldSlot));
    }pending.reset();
}
void resetRound(std::uint64_t world){
    // Cross-world pointers are discarded; never restore into an expired world.
    pending.reset();probe.reset();reader.clear();model=std::make_unique<sb::Model>(*config.load());model->reset(world);
    roundConfig=config.load();currentTitle.clear();currentSubtitle.clear();lastQueuedPass=capturePass=acceptedResult=0;lastPhase=sb::Phase::idle;
    {std::unique_lock lock(diffMutex,std::try_to_lock);if(lock){diffInput.reset();diffOutput.reset();}}
}
void processTitles(const native::NativeHotbar& hotbar,const Vec& position){
    std::deque<TitleCopy> updates;{std::unique_lock lock(titleMutex,std::try_to_lock);if(lock){updates.swap(titles);titleQueued.fetch_sub(static_cast<unsigned>(updates.size()));}}
    for(const auto& update:updates){if(update.epoch!=generation)continue;
        if(update.type==0||update.type==1){currentTitle.clear();currentSubtitle.clear();}
        else if(update.type==2||update.type==6){currentTitle=titleText(update.text,update.type);if(tick-subtitleTick>10)currentSubtitle.clear();
            model->onPrompt(generation,tick,{},hotbar.copied,currentTitle,{});}
        else if(update.type==3||update.type==7){currentSubtitle=titleText(update.text,update.type);subtitleTick=tick;}
        const sb::BlockPos center{static_cast<int>(std::floor(position.x)),static_cast<int>(std::floor(position.y)),static_cast<int>(std::floor(position.z))};
        if(model->onPrompt(generation,tick,center,hotbar.copied,currentTitle,currentSubtitle)){
            probe.reset();restorePending(playerIdentity,runtime);lastQueuedPass=0;
            note("memorizing: topic="+currentTitle+" subtitle="+currentSubtitle+" radius="+std::to_string(roundConfig->radius));
        }
    }
}
void capture(void* source){
    const auto request=model->nextCapture(tick);if(request.positions.empty())return;
    const auto deadline=Clock::now()+std::chrono::microseconds(1000);std::vector<sb::Cell> cells;cells.reserve(request.positions.size());
    for(const auto& pos:request.positions){cells.push_back(reader.cell(source,pos));if(Clock::now()>=deadline)break;}
    if(!model->submitCapture(request.token,tick,cells))return;
    const auto stats=model->stats();capturePass=stats.capturePasses;
    if(model->phase()!=sb::Phase::waiting||!model->previewFrozen()||capturePass==lastQueuedPass)return;
    auto before=model->previewSnapshot();auto after=model->observationSnapshot(true);if(!before||!after)return;
    // Publish at most one immutable copied snapshot per completed pass.
    auto job=std::make_shared<DiffJob>(DiffJob{std::move(*before),std::move(*after),++jobSequence});
    std::unique_lock lock(diffMutex,std::try_to_lock);if(lock){diffInput=std::move(job);lastQueuedPass=capturePass;}
}
bool probeNext(void* player,void* source,const native::NativeHotbar& hotbar,Vec eye){
    if(!probe)return false;auto& p=*probe;
    if(!model->refreshPlacement(p.request.token,p.request.actionId,tick)){probe.reset();return false;}
    const auto slot=p.request.hotbarSlot;const auto& target=p.request.target;
    const auto now=reader.cell(source,target);if(now.kind!=sb::CellKind::air){model->deferPlacement(p.request.token,p.request.actionId,tick);probe.reset();return false;}
    auto state=reader.states.find(reinterpret_cast<void*>(p.targetState.value_or(0)));
    if(state==reader.states.end()||slot>=9||hotbar.types[slot]!=state->second.type||!hotbar.copied[slot].count){model->deferPlacement(p.request.token,p.request.actionId,tick);probe.reset();return false;}
    // Every proposal is independently checked against the engine's immutable target state.
    // Six faces * four cardinal yaw values * three heights * three pitch values.
    constexpr unsigned faces[6]={1,2,3,4,5,0};constexpr float heights[3]={.5f,.25f,.75f};constexpr float pitches[3]={0.f,90.f,-90.f};
    const auto deadline=Clock::now()+std::chrono::microseconds(350);
    for(unsigned step=0;step<8&&p.candidate<216;++step){const unsigned n=p.candidate++;unsigned i=n;
        const auto face=faces[i%6];i/=6;const float yaw=float(i%4)*90.f;i/=4;const float height=heights[i%3];i/=3;const float pitch=pitches[i%3];
        const auto normal=sb::faceNormal(static_cast<sb::Face>(face));sb::BlockPos support{target.x-normal.x,target.y-normal.y,target.z-normal.z};
        const auto base=reader.cell(source,support);if(base.kind!=sb::CellKind::block||!base.supportsPlacement)continue;
        Vec fractional{.5f,height,.5f};if(face==0)fractional.y=0;else if(face==1)fractional.y=1;
        else if(face==2)fractional.z=0;else if(face==3)fractional.z=1;else if(face==4)fractional.x=0;else fractional.x=1;
        Vec hit{float(support.x)+fractional.x,float(support.y)+fractional.y,float(support.z)+fractional.z};
        const double dx=double(hit.x)-eye.x,dy=double(hit.y)-eye.y,dz=double(hit.z)-eye.z;
        if(dx*dx+dy*dy+dz*dz>roundConfig->maxReach*roundConfig->maxReach)continue;
        void* predicted{};
        if(bindings.predict(hotbar.types[slot],player,target,face,fractional,hotbar.aux[slot],yaw,pitch,&predicted)&&reinterpret_cast<std::uintptr_t>(predicted)==p.targetState){
            Pending action;action.request=p.request;action.request.support=support;action.request.recipe.face=static_cast<sb::Face>(face);
            action.request.recipe.orientationKnown=action.request.recipe.exactStateSupported=true;
            action.request.recipe.yaw=yaw;action.request.recipe.pitch=pitch;action.request.recipe.hitX=fractional.x;action.request.recipe.hitY=fractional.y;action.request.recipe.hitZ=fractional.z;
            action.runtime=runtime;action.player=player;action.source=source;action.prepared=tick;action.yaw=yaw;action.pitch=pitch;action.hit=hit;action.oldSlot=bindings.selected(player);
            auto* rot=bindings.component(player,bindings.actors.actorRotationComponentHash);
            if(native::read(rot,bindings.actors.rotationYawOffset,action.oldYaw)&&native::read(rot,bindings.actors.rotationPitchOffset,action.oldPitch)&&
               bindings.select(player,slot)&&native::write(rot,bindings.actors.rotationYawOffset,yaw)&&native::write(rot,bindings.actors.rotationPitchOffset,pitch)){
                pending=action;probe.reset();return true;
            }
            // Undo even a partially prepared action.
            pending=action;restorePending(player,runtime);model->placementResult(p.request.token,p.request.actionId,false,tick);probe.reset();return false;
        }
        if(Clock::now()>=deadline)break;
    }
    if(p.candidate>=216){note("placement unresolved: "+p.request.recipe.itemIdentifier+" target="+std::to_string(target.x)+","+std::to_string(target.y)+","+std::to_string(target.z));
        model->placementResult(p.request.token,p.request.actionId,false,tick);probe.reset();}
    return false;
}
void tickGame(const bedrock::LevelTickEvent& event){
    void* instance=client.load();void* player=bindings.local(instance);void* level{};
    if(!player||!native::read(player,0x1d8,level)||level!=event.level)return;
    Vec position{};std::uint64_t id{};if(!bindings.pose(player,position,id))return;
    auto* source=bindings.region(instance);if(!source)return;
    const auto world=epoch.load();
    if(!model||generation!=world||playerIdentity!=player||runtime!=id||sourceIdentity!=source){
        generation=world;playerIdentity=player;runtime=id;sourceIdentity=source;seenConfig=configRevision.load();resetRound(world);note("world identity ready; recording/build caches reset");
    }
    ++tick;
    if(seenConfig!=configRevision.load()){restorePending(player,id);seenConfig=configRevision.load();resetRound(world);note("config applied; round cancelled");}
    model->update(generation,tick);
    if(stopped.load()||cancelRequested.exchange(false)||!gameplay.load()||!roundConfig->enabled){restorePending(player,id);probe.reset();if(model->phase()!=sb::Phase::idle&&model->phase()!=sb::Phase::cancelled)model->cancel();return;}
    if((model->phase()==sb::Phase::idle||model->phase()==sb::Phase::complete||model->phase()==sb::Phase::cancelled||model->phase()==sb::Phase::failed)&&!titleQueued.load())return;
    native::NativeHotbar hotbar;if(!reader.hotbar(player,hotbar)){restorePending(player,id);probe.reset();model->cancel();note("round cancelled: hotbar read failed");return;}
    processTitles(hotbar,position);
    if(model->phase()==sb::Phase::capturing||model->phase()==sb::Phase::waiting)capture(source);
    if(model->phase()==sb::Phase::waiting){std::shared_ptr<const DiffResult> ready;
        {std::unique_lock lock(diffMutex,std::try_to_lock);if(lock)ready=diffOutput;}
        if(ready&&ready->serial!=acceptedResult&&ready->candidates.token==model->token()){
            if(model->acceptCandidates(ready->candidates,tick,hotbar.copied)){acceptedResult=ready->serial;note("building: removed preview blocks="+std::to_string(model->stats().queued)+" permanent scenery excluded="+std::to_string(model->stats().persistent));}
        }
    }
    Vec eye=position; // StateVector position is the native Player eye origin in this exact build.
    if(pending){
        if(pending->dispatched){restorePending(player,id);}
        else if(tick>pending->prepared){
            auto& action=*pending;const auto token=action.request.token;const auto aid=action.request.actionId;
            float yaw{},pitch{};auto* rot=bindings.component(player,bindings.actors.actorRotationComponentHash);
            const auto target=reader.cell(source,action.request.target);const auto support=reader.cell(source,action.request.support);void* predicted{};
            const auto desired=model->recordedState(action.request.target).value_or(0);
            const auto& recipe=action.request.recipe;const unsigned slot=action.request.hotbarSlot;
            const double dx=double(action.hit.x)-eye.x,dy=double(action.hit.y)-eye.y,dz=double(action.hit.z)-eye.z;
            const bool valid=model->actionPending(token,aid)&&action.runtime==id&&action.source==source&&
                bindings.selected(player)==static_cast<int>(slot)&&hotbar.copied[slot].count&&hotbar.copied[slot].identifier==recipe.itemIdentifier&&
                native::read(rot,bindings.actors.rotationYawOffset,yaw)&&native::read(rot,bindings.actors.rotationPitchOffset,pitch)&&
                std::abs(yaw-action.yaw)<.01f&&std::abs(pitch-action.pitch)<.01f&&target.kind==sb::CellKind::air&&support.supportsPlacement&&
                dx*dx+dy*dy+dz*dz<=roundConfig->maxReach*roundConfig->maxReach&&
                bindings.predict(hotbar.types[slot],player,action.request.target,static_cast<unsigned>(recipe.face),{recipe.hitX,recipe.hitY,recipe.hitZ},hotbar.aux[slot],yaw,pitch,&predicted)&&reinterpret_cast<std::uintptr_t>(predicted)==desired;
            if(valid){bool accepted=false;auto* gm=bindings.gameMode(player);const bool dispatched=gm&&bindings.build(player,gm,action.request.support,static_cast<unsigned>(recipe.face),eye,action.hit,&accepted);
                model->placementResult(token,aid,dispatched&&accepted,tick);action.dispatched=true;
                if(!dispatched)note("placement rejected: native game mode/HitResult validation failed");
            }else {model->deferPlacement(token,aid,tick);restorePending(player,id);}
        }
    }
    if(model->phase()==sb::Phase::building&&!pending){
        if(!probe){sb::BuildFrame frame;frame.world=generation;frame.tick=tick;frame.eye={eye.x,eye.y,eye.z};frame.hotbar=hotbar.copied;
            for(const auto& pos:model->inspectionPositions())frame.cells.push_back(reader.cell(source,pos));
            if(auto request=model->nextPlacement(frame)){
                const auto desired=model->recordedState(request->target);
                if(desired)probe=Probe{*request,desired,0};else model->deferPlacement(request->token,request->actionId,tick);
            }
        }
        if(probe)probeNext(player,source,hotbar,eye);
    }
    const auto phase=model->phase();
    if(phase!=lastPhase||tick-lastProgress>=100){lastPhase=phase;lastProgress=tick;
        if(phase!=sb::Phase::idle&&phase!=sb::Phase::cancelled){const auto stats=model->stats();note("progress: phase="+std::to_string(static_cast<int>(phase))+" captured="+std::to_string(stats.captured)+" observed="+std::to_string(stats.confirmed)+" remaining="+std::to_string(stats.remaining)+" exhausted="+std::to_string(stats.exhausted));}}
    if(phase==sb::Phase::complete||phase==sb::Phase::failed||phase==sb::Phase::cancelled){restorePending(player,id);probe.reset();}
}
void onTick(const bedrock::LevelTickEvent& event){try{tickGame(event);}catch(const std::exception& e){cancelRequested.store(true);note(std::string("tick failed: ")+e.what());}}
std::filesystem::path directory(){PWSTR path{};if(FAILED(SHGetKnownFolderPath(FOLDERID_Documents,KF_FLAG_CREATE,nullptr,&path)))throw std::runtime_error("Documents directory unavailable");
    auto dir=std::filesystem::path(path)/L"PEXData"/L"SpeedBuilder";CoTaskMemFree(path);return dir;}
void worker(const std::filesystem::path& dir){
    std::ofstream file(dir/L"SpeedBuilder.log",std::ios::trunc);file<<"SpeedBuilder 1.26.52.3 candidate; native reads/writes on client tick; copied diff worker\n";
    const auto path=dir/L"config.json";auto modified=std::filesystem::last_write_time(path);auto nextReload=Clock::now();
    while(!stopped.load()){
        std::shared_ptr<const DiffJob> job;{std::lock_guard lock(diffMutex);job=std::move(diffInput);diffInput.reset();}
        if(job&&job->before.token.world==epoch.load()){
            auto result=std::make_shared<DiffResult>(DiffResult{sb::prepareCandidates(job->before,job->after),job->serial});
            // A fresh second removal pass prevents acting on an incomplete disappearance animation.
            static std::vector<sb::Cell> previous;static sb::SessionToken previousToken{};
            const bool stable=previousToken==result->candidates.token&&previous==result->candidates.removed;
            previousToken=result->candidates.token;previous=result->candidates.removed;
            if(stable&&result->candidates.token.world==epoch.load()){std::lock_guard lock(diffMutex);diffOutput=std::move(result);}
        }
        std::deque<std::string> batch;{std::lock_guard lock(logMutex);batch.swap(logs);}
        for(const auto& line:batch)file<<line<<'\n';if(!batch.empty())file.flush();
        if(auto lost=lostTitles.exchange(0))file<<"title mailbox dropped="<<lost<<"; cancel current round if title was missed\n";
        if(auto lost=lostLogs.exchange(0))file<<"log queue dropped="<<lost<<'\n';
        if(Clock::now()>=nextReload){nextReload=Clock::now()+std::chrono::milliseconds(500);std::error_code ec;const auto current=std::filesystem::last_write_time(path,ec);
            if(!ec&&current!=modified){modified=current;try{std::ifstream in(path);std::string text((std::istreambuf_iterator<char>(in)),{});auto next=std::make_shared<const sb::Config>(sb::parseConfig(text));config.store(std::move(next));++configRevision;file<<"config ready\n";}catch(const std::exception& e){file<<"config rejected: "<<e.what()<<'\n';}}}
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
DWORD WINAPI start(void*){
    HMODULE pinned{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&start),&pinned);status.store(1);
    try{
        const auto dir=directory();std::filesystem::create_directories(dir);const auto path=dir/L"config.json";
        if(!std::filesystem::exists(path)){std::ofstream out(path);out<<sb::configJson(sb::Config{}).dump(2)<<'\n';}
        {std::ifstream in(path);std::string text((std::istreambuf_iterator<char>(in)),{});config.store(std::make_shared<const sb::Config>(sb::parseConfig(text)));}
        std::string profile,error;bedrock::ProfileChoice choice;if(!bedrock::fetchSupportedProfile(profile,choice,error))throw std::runtime_error(error);
        if(!nlohmann::json::parse(profile).contains("native_speed_builder"))profile=bedrock::generated::kSymbolDatabase;
        bedrock::Resolver resolver;if(!resolver.loadJson(profile,error)||!resolver.attachWindowsMainModule(choice.identity,error)||!resolver.exactBuild())throw std::runtime_error("exact-build attach failed: "+error);
        bindings.load(profile);
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
