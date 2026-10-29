#include <bedrock/profile_json.hpp>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <bedrock/game_api.hpp>
#include <bedrock/native_motion.hpp>
#include <bedrock/native_packet_sender.hpp>
#include <bedrock/wire_packet_events.hpp>
#include <bedrock/movement_packets.hpp>
#include <bedrock/native_brightness.hpp>
#include <bedrock/native_camera.hpp>
#include <bedrock/native_game_type.hpp>
#include <bedrock/game_hooks.hpp>
#include <bedrock/generated_symbols.hpp>
#include <bedrock/native_actors.hpp>
#include <bedrock/native_items.hpp>
#include <bedrock/native_world_render.hpp>
#include <bedrock/remote_catalog.hpp>
#include <bedrock/profile_manager.hpp>
#include <entt/entt.hpp>
#include "freecam/config.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#if !defined(_MSC_VER) || _ITERATOR_DEBUG_LEVEL != 0
#error "Freecam requires MSVC Release STL layout."
#endif
namespace freecam_native {
struct Entity { std::uint32_t value; constexpr operator std::uint32_t() const {return value;} constexpr bool operator==(const Entity&) const=default; };
struct Traits {using value_type=Entity;using entity_type=std::uint32_t;using version_type=std::uint16_t;
    static constexpr entity_type entity_mask=0x3ffff;static constexpr version_type version_mask=0x3fff;};
}
namespace entt {template<> struct entt_traits<freecam_native::Entity>:basic_entt_traits<freecam_native::Traits>{static constexpr std::size_t page_size=2048;};}
namespace {
using Vec=bedrock::Vec3; using Clock=std::chrono::steady_clock;
using Registry=entt::basic_registry<freecam_native::Entity>;
struct Box {std::uint64_t id{};bedrock::ActorBounds bounds{},blendFrom{};};
struct Capture {std::uint64_t epoch{};Clock::time_point made{};std::array<Box,128> boxes{};unsigned count{};Vec origin{};};
struct Mesh {std::uint64_t epoch{};Clock::time_point made{};unsigned count{};std::array<Vec,128*24> vertices{},fromVertices{};};
std::atomic<bool> stop{},desired{},active{},gameplay{},restoreRequested{};
std::atomic<unsigned> status{};
unsigned visualRevision=9;std::string profileGameVersion,profileExeSha;
std::atomic<void*> client{},local{};
std::atomic<std::uint64_t> epoch{1};
std::array<std::atomic<bool>,256> keys{};
std::atomic<std::shared_ptr<const freecam::Config>> config{std::make_shared<const freecam::Config>()};
bedrock::GameApi api;std::unique_ptr<bedrock::GameHooks> hooks;
bedrock::NativeActorLayout actors;bedrock::WorldRenderProfile render;
std::size_t hurtOffset{};std::uint32_t hurtHash{};
BDS_HookToken renderToken{};
bedrock::NativeBrightness brightness;bedrock::NativeCamera nativeCamera;bedrock::NativeGameType gameTypes;
using CameraRenderFn=void(__fastcall*)(void*,void*,void*,void*);CameraRenderFn originalCameraRender{};
BDS_HookToken cameraRenderToken{};std::atomic<bool> cameraRenderReady{};
using CreateViewFn=void*(__fastcall*)(void*,void*,void*,unsigned char);CreateViewFn originalCreateView{};
BDS_HookToken createViewToken{};std::atomic<bool> createViewReady{};
using EffectPredicateFn=bool(__fastcall*)(void*,void*);EffectPredicateFn originalEffectPredicate{};
BDS_HookToken effectToken{};std::atomic<bool> effectReady{};
std::array<std::uintptr_t,2> effectTables{},effectCallers{};
std::atomic<unsigned> effectCalls{},effectEligible{},effectSkipped{},effectOtherCaller{};
std::atomic<std::uintptr_t> effectLastCaller{};
std::atomic<unsigned> cameraRenderCalls{},viewCreated{},viewChanged{},viewWriteFailed{};

// All culler ownership bookkeeping below belongs to the native render callback.
// Workers only read atomic diagnostics; they never access camera/culler pointers.
using SetCullerFn=void(__fastcall*)(void*,unsigned char);
SetCullerFn setCuller{},originalSetCuller{};BDS_HookToken cullerToken{};
std::size_t cameraCullerOffset{},cameraCullerTypeOffset{};
std::uintptr_t spectatorCullerVtable{};
std::atomic<bool> terrainReady{};
std::atomic<unsigned> terrainCalls{},terrainChanges{},terrainRestores{},terrainFaults{};
std::atomic<int> terrainObservedMode{-1};std::atomic<bool> terrainPending{};
struct CullerLease{void* camera{};std::uint64_t epoch{};unsigned char previous{};bool owned{};std::uint64_t nativeModeContext{};};
CullerLease cullerLease;std::mutex terrainLeaseMutex;
std::atomic<std::uint64_t> nativeModeContext{};std::atomic<bool> terrainLeaseActive{};
constexpr unsigned char spectatorCullerMode=5;
std::atomic<void*> primaryCamera{};std::atomic<std::uint64_t> primaryCameraEpoch{},flightSerial{};
std::atomic<unsigned> selectorRequests{},selectorRewrites{};std::atomic<std::uintptr_t> selectorCaller{};
std::atomic<bool> terrainRejected{};std::uint64_t budgetFlight{};unsigned budgetSwitches{};
void resetCullerBudgetLocked(){const auto serial=flightSerial.load();if(serial!=budgetFlight){budgetFlight=serial;budgetSwitches=0;terrainRejected.store(false);}}
void recordCullerSwitchLocked(){terrainChanges.fetch_add(1,std::memory_order_relaxed);if(++budgetSwitches>=8)terrainRejected.store(true);}

std::atomic<bool> gammaReady{};

std::mutex mailboxMutex,logMutex;Capture mailbox{};bool mailboxReady{};
std::atomic<std::shared_ptr<const Mesh>> mesh;
std::ofstream logFile;
std::atomic<unsigned> logBits{};
enum :unsigned {Activated=1,Disabled=2,Damage=4,ReadFailed=8,World=16,RenderFault=32,Waiting=64};
void note(unsigned flag){logBits.fetch_or(flag,std::memory_order_relaxed);}
void log(std::string_view text){std::lock_guard guard(logMutex);if(logFile){logFile<<text<<'\n';logFile.flush();}}
bool finite(Vec v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z)&&std::abs(v.x)<40000000&&std::abs(v.y)<40000000&&std::abs(v.z)<40000000;}
template<class T> bool read(const void* p,std::size_t offset,T& out) noexcept {
    if(!p||reinterpret_cast<std::uintptr_t>(p)<0x10000) return false;
    __try {std::memcpy(&out,static_cast<const std::byte*>(p)+offset,sizeof(T));return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
template<class T> bool write(void* p,std::size_t offset,const T& value) noexcept {
    if(!p)return false;
    __try {std::memcpy(static_cast<std::byte*>(p)+offset,&value,sizeof(T));return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
// Opt-in bounded observations. Native reads remain on the owning callbacks;
// the worker serializes only scalar copies. These fields do not alter rendering.
struct VisibilitySample {std::uint64_t epoch{},milliseconds{};int gameType=-1;bool flying{},valid{};
    Vec camera654{},origin{};std::array<unsigned char,7> fogFlags{};unsigned char cullerType{};std::uintptr_t cullerVtable{};
    bool viewValid{};std::array<unsigned char,13> viewFlags{};};
std::mutex visibilityMutex;VisibilitySample visibilityMailbox{};bool visibilityReady{};
std::atomic<int> diagnosticGameType{-1};std::atomic<std::uint64_t> diagnosticGameEpoch{},diagnosticGameTime{};
void captureVisibility(void* renderer,void* view=nullptr,bool directCamera=false){
    if(!config.load()->visibilityDiagnostics||stop.load())return;
    static thread_local Clock::time_point next{};const auto now=Clock::now();if(now<next)return;
    next=now+std::chrono::milliseconds(500);
    VisibilitySample s;s.epoch=epoch.load();s.milliseconds=std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();s.flying=active.load();
    if(diagnosticGameEpoch.load()==s.epoch&&s.milliseconds-diagnosticGameTime.load()<=1500)s.gameType=diagnosticGameType.load();
    void* camera=directCamera?renderer:nullptr;void* culler{};std::uintptr_t vt{};
    if(!directCamera&&!read(renderer,render.player,camera))return;
    s.valid=camera&&read(camera,0x654,s.camera654)&&finite(s.camera654)&&
        read(camera,render.origin,s.origin)&&finite(s.origin)&&read(camera,0x970,s.fogFlags)&&
        read(camera,0x9b8,s.cullerType)&&s.cullerType<=5&&read(camera,0x9a8,culler)&&read(culler,0,vt);
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if(vt>=base&&vt-base<0x20000000)s.cullerVtable=vt-base;
    s.viewValid=view&&read(view,0x7c,s.viewFlags)&&std::all_of(s.viewFlags.begin(),s.viewFlags.end(),[](unsigned char b){return b<=1;});
    std::unique_lock lock(visibilityMutex,std::try_to_lock);if(lock){visibilityMailbox=s;visibilityReady=true;}
}
// A value-returning native factory owns this output until it returns to its
// caller. Change only its newly constructed bool, never an already shared view.
void* __fastcall createViewDetour(void* camera,void* output,void* screen,unsigned char subClient){
    const auto world=epoch.load();
    void* result=originalCreateView(camera,output,screen,subClient);
    if(camera!=primaryCamera.load()||primaryCameraEpoch.load()!=world||epoch.load()!=world)return result;
    viewCreated.fetch_add(1,std::memory_order_relaxed);
    if(!active.load()||stop.load()||!config.load()->spectatorMode)return result;
    std::array<unsigned char,13> flags{};
    if(result!=output||!read(output,0x7c,flags)||!std::all_of(flags.begin(),flags.end(),[](unsigned char b){return b<=1;})){
        viewWriteFailed.fetch_add(1,std::memory_order_relaxed);return result;
    }
    if(flags[4]){
        const unsigned char disabled=0;
        if(write(output,0x80,disabled))viewChanged.fetch_add(1,std::memory_order_relaxed);
        else viewWriteFailed.fetch_add(1,std::memory_order_relaxed);
    }
    return result;
}
// Read-only observation after the factory, at the native camera consumer.
void __fastcall cameraRenderDetour(void* camera,void* context,void* view,void* instance){
    if(camera==primaryCamera.load()&&primaryCameraEpoch.load()==epoch.load()){
        cameraRenderCalls.fetch_add(1,std::memory_order_relaxed);captureVisibility(camera,view,true);
    }
    originalCameraRender(camera,context,view,instance);
}
// RenderDragon extraction predicate. This shared native implementation serves
// many tags, so require BOTH the InsideBlockEffect callable table and its exact
// extraction call site. No frame/entity/terrain data is mutated.
bool __fastcall effectPredicateDetour(void* predicate,void* arguments){
    const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    std::uintptr_t table{};
    if(!read(predicate,0,table))return originalEffectPredicate(predicate,arguments);
    const bool tagged=table==effectTables[0]||table==effectTables[1];
    if(!tagged)return originalEffectPredicate(predicate,arguments);
    const bool permitted=originalEffectPredicate(predicate,arguments);
    effectCalls.fetch_add(1,std::memory_order_relaxed);
    const auto image=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if(caller>=image&&caller-image<0x20000000)effectLastCaller.store(caller-image);
    if(caller!=effectCallers[0]&&caller!=effectCallers[1]){
        effectOtherCaller.fetch_add(1,std::memory_order_relaxed);return permitted;
    }
    if(permitted)effectEligible.fetch_add(1,std::memory_order_relaxed);
    if(permitted&&effectReady.load(std::memory_order_relaxed)&&active.load()&&!stop.load()&&gameplay.load()&&
       primaryCameraEpoch.load()==epoch.load()&&config.load()->spectatorMode){
        effectSkipped.fetch_add(1,std::memory_order_relaxed);return false;
    }
    return permitted;
}
// Intercept the native selector BEFORE its update. Repeated native requests
// then see an already selected mode5 and do not rebuild the async culler each frame.
void __fastcall cullerDetour(void* camera,unsigned char requested){
    const auto world=epoch.load();const auto context=nativeModeContext.load();
    bool rewrite=terrainReady.load()&&requested==3&&camera==primaryCamera.load()&&primaryCameraEpoch.load()==world&&
        (context>>8)==world&&(context&0xff)!=7&&active.load()&&!stop.load()&&config.load()->spectatorMode;
    if(!rewrite){originalSetCuller(camera,requested);return;}
    selectorRequests.fetch_add(1,std::memory_order_relaxed);
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if(caller>=base&&caller-base<0x20000000)selectorCaller.store(caller-base);
    std::unique_lock guard(terrainLeaseMutex,std::try_to_lock);
    if(!guard){originalSetCuller(camera,requested);return;}
    resetCullerBudgetLocked();
    if(terrainRejected.load()){originalSetCuller(camera,requested);return;}
    unsigned char before{};if(!read(camera,cameraCullerTypeOffset,before)||before>5){originalSetCuller(camera,requested);return;}
    if(cullerLease.owned&&(cullerLease.camera!=camera||cullerLease.epoch!=world||cullerLease.nativeModeContext!=context))cullerLease={};
    if(!cullerLease.owned){cullerLease={camera,world,requested,true,context};terrainLeaseActive.store(true);}
    selectorRewrites.fetch_add(1,std::memory_order_relaxed);
    originalSetCuller(camera,spectatorCullerMode);
    unsigned char after{};if(read(camera,cameraCullerTypeOffset,after)){
        terrainObservedMode.store(after);terrainPending.store(after!=spectatorCullerMode);
        if(before!=spectatorCullerMode&&after==spectatorCullerMode)recordCullerSwitchLocked();
    }
}
bool invokeCuller(void* camera,unsigned char mode) noexcept {
    if(!setCuller)return false;
    __try{setCuller(camera,mode);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void updateTerrainCulling(void* renderer){
    if(!terrainReady.load(std::memory_order_relaxed))return;
    std::unique_lock leaseLock(terrainLeaseMutex,std::try_to_lock);if(!leaseLock)return;
    resetCullerBudgetLocked();
    const auto world=epoch.load();void* camera{};unsigned char mode{};
    if(cullerLease.epoch!=world){cullerLease={};terrainLeaseActive.store(false);}
    if(!read(renderer,render.player,camera)||!camera||!read(camera,cameraCullerTypeOffset,mode)||mode>5)return;
    if(cullerLease.camera&&cullerLease.camera!=camera){cullerLease={};terrainLeaseActive.store(false);}
    const auto nativeContext=nativeModeContext.load();
    // A native game-mode change surrenders ownership to the engine. In
    // particular, never restore mode3 over a newly entered real spectator.
    if(cullerLease.owned&&nativeContext&&nativeContext!=cullerLease.nativeModeContext){cullerLease={};terrainLeaseActive.store(false);}
    terrainObservedMode.store(mode,std::memory_order_relaxed);
    const bool want=active.load(std::memory_order_relaxed)&&!stop.load(std::memory_order_relaxed)&&config.load()->spectatorMode&&!terrainRejected.load();
    // Native setter checks that its asynchronous culler is idle, constructs
    // the replacement and resets visibility caches. A busy culler is retried
    // on a later frame; no spin/wait and no manual native shared_ptr swap.
    if(want){
        if(mode!=3&&mode!=spectatorCullerMode){terrainPending.store(true);return;}
        if(mode==spectatorCullerMode){
            void* culler{};std::uintptr_t table{};
            const bool expected=read(camera,cameraCullerOffset,culler)&&read(culler,0,table)&&table==spectatorCullerVtable;
            terrainPending.store(!expected,std::memory_order_relaxed);return;
        }
        if((nativeContext>>8)!=world){terrainPending.store(true);return;}
        if(!cullerLease.owned){cullerLease={camera,world,mode,true,nativeContext};terrainLeaseActive.store(true);}
        if(epoch.load()!=world)return;
        terrainCalls.fetch_add(1,std::memory_order_relaxed);
        if(!invokeCuller(camera,spectatorCullerMode)){terrainFaults.fetch_add(1);terrainReady.store(false);return;}
        unsigned char after{};
        if(read(camera,cameraCullerTypeOffset,after)){
            terrainObservedMode.store(after,std::memory_order_relaxed);
            if(after==spectatorCullerMode)recordCullerSwitchLocked();
            terrainPending.store(after!=spectatorCullerMode,std::memory_order_relaxed);
        }
    }else if(cullerLease.owned){
        // A different mode means another native caller has already changed it;
        // do not overwrite that caller's choice with a stale saved value.
        if(mode!=spectatorCullerMode){if(mode==cullerLease.previous)terrainRestores.fetch_add(1);cullerLease={};terrainLeaseActive.store(false);terrainPending.store(false);return;}
        if(epoch.load()!=world)return;
        if(!invokeCuller(camera,cullerLease.previous)){terrainFaults.fetch_add(1);terrainReady.store(false);return;}
        unsigned char after{};
        if(read(camera,cameraCullerTypeOffset,after)){
            terrainObservedMode.store(after,std::memory_order_relaxed);
            if(after==cullerLease.previous){cullerLease={};terrainLeaseActive.store(false);terrainRestores.fetch_add(1);terrainPending.store(false);}
            else terrainPending.store(true);
        }
    }else terrainPending.store(false,std::memory_order_relaxed);
}
void disable(unsigned reason=Disabled){brightness.reset();restoreRequested.store(true);desired.store(false);active.store(false);mesh.store(nullptr);note(reason);}
void reset(){disable(World);nativeCamera.reset();gameTypes.reset();epoch.fetch_add(1);local.store(nullptr);for(auto& key:keys)key.store(false);}
void* component(void* actor,std::uint32_t hash) noexcept {
    void* registry{};std::uint32_t id{};
    if(!read(actor,actors.actorEntityContextOffset+actors.contextRegistryOffset,registry)||
       !read(actor,actors.actorEntityContextOffset+actors.contextEntityIdOffset,id)||!registry) return nullptr;
    __try {auto* r=static_cast<Registry*>(registry);const freecam_native::Entity e{id};auto* s=r->storage(hash);
        return r->valid(e)&&s&&s->contains(e)?s->value(e):nullptr;}
    __except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
}
bool hurtTime(void* actor,int& value) noexcept {return read(component(actor,hurtHash),hurtOffset,value)&&value>=0&&value<10000;}
void observePacketSender(void*) noexcept;
bool isMovement(unsigned key,const freecam::Config& c){return key==c.forward||key==c.back||key==c.left||key==c.right||key==c.up||key==c.down||key==c.fast;}
thread_local bool releasing=false;
void releaseMovement(const freecam::Config& c){
    releasing=true;
    for(unsigned key:{c.forward,c.back,c.left,c.right,c.up,c.down,c.fast}){
        BDS_KeyboardArgs args{static_cast<int>(key),0,0};BDS_HookCallOriginal(BDS_HOOK_KEYBOARD_FEED,&args);
    }
    api.sendMouseInput(1,false);api.sendMouseInput(2,false);releasing=false;
}
void keyEvent(const bedrock::KeyEvent& e){
    if(releasing||stop.load())return;const auto c=config.load();const unsigned k=e.inputKey();if(k<keys.size())keys[k].store(e.pressed);
    if(k==c->toggleKey){
        if(e.pressed&&!e.repeat&&gameplay.load()&&c->enabled){
            if(desired.load())disable();else if(!restoreRequested.load()){releaseMovement(*c);desired.store(true);}
        }
        if(gameplay.load()) e.cancel();return;
    }
    if(desired.load()&&isMovement(k,*c))e.cancel();
}
void mouseEvent(const bedrock::MouseEvent& e){
    if(releasing||stop.load()||!desired.load())return;const auto c=config.load();
    if(e.action==0)return;
    else if(!c->allowInteraction && (e.action==1||e.action==2||e.action==3||e.action==4))e.cancel();
}
bool capturePlayers(void* actor,Capture* batch,unsigned limit) noexcept {
    // The game thread owns these generation-checked handles. Workers receive
    // only copied AABBs; discovery stays bounded while known Players update each tick.
    struct Tracked {freecam_native::Entity entity{};std::uint64_t id{};bedrock::ActorBounds bounds{};Clock::time_point seen{};};
    static thread_local std::array<Tracked,128> tracked{};
    static thread_local std::size_t trackedCount=0,cursor=0;
    static thread_local void* previousRegistry{};static thread_local std::uint64_t previousEpoch{};
    void* registry{};if(!read(actor,actors.actorEntityContextOffset+actors.contextRegistryOffset,registry)||!registry)return false;
    if(previousRegistry!=registry||previousEpoch!=batch->epoch){trackedCount=cursor=0;previousRegistry=registry;previousEpoch=batch->epoch;}
    batch->made=Clock::now();batch->count=0;
    __try {
        auto* r=static_cast<Registry*>(registry);auto* types=r->storage(actors.actorTypeComponentHash);
        auto* owners=r->storage(actors.actorOwnerComponentHash);auto* bounds=r->storage(actors.aabbShapeComponentHash);
        auto* ids=r->storage(actors.runtimeIdComponentHash);if(!types||!owners||!bounds||!ids)return false;
        const auto size=types->size();if(size>16384)return false;const auto start=Clock::now();
        for(unsigned n=0;n<limit&&n<size;++n){
            if(n&&Clock::now()-start>std::chrono::microseconds(150))break;
            if(cursor>=size)cursor=0;const auto entity=types->data()[cursor++];std::uint32_t type{};void* owner{};std::uint64_t id{};
            if(!r->valid(entity)||!types->contains(entity)||!read(types->value(entity),render.actorTypeValueOffset,type)||type!=render.playerTypeId||
               !owners->contains(entity)||!read(owners->value(entity),actors.actorOwnerPointerOffset,owner)||!owner||owner==actor||
               !ids->contains(entity)||!read(ids->value(entity),actors.runtimeIdValueOffset,id)||!id)continue;
            std::size_t i=0;for(;i<trackedCount;++i)if(tracked[i].id==id)break;
            if(i==trackedCount && trackedCount<tracked.size())tracked[trackedCount++]={entity,id};
        }
        for(std::size_t i=0;i<trackedCount;){
            auto& item=tracked[i];std::uint64_t id{};void* owner{};Box box;
            if(!r->valid(item.entity)||!ids->contains(item.entity)||!owners->contains(item.entity)||
               !read(ids->value(item.entity),actors.runtimeIdValueOffset,id)||id!=item.id||
               !read(owners->value(item.entity),actors.actorOwnerPointerOffset,owner)||!owner||owner==actor||
               !bounds->contains(item.entity)) {tracked[i]=tracked[--trackedCount];continue;}
            auto* shape=bounds->value(item.entity);
            if(!read(shape,actors.aabbMinimumOffset,box.bounds.minimum)||!read(shape,actors.aabbMaximumOffset,box.bounds.maximum)||
               !box.bounds.valid()||!finite(box.bounds.minimum)||!finite(box.bounds.maximum)) {tracked[i]=tracked[--trackedCount];continue;}
            const auto a=box.bounds.minimum,b=box.bounds.maximum;
            if(b.x-a.x>4||b.y-a.y>5||b.z-a.z>4){tracked[i]=tracked[--trackedCount];continue;}
            box.id=id;box.blendFrom=box.bounds;
            const float dx=a.x-item.bounds.minimum.x,dy=a.y-item.bounds.minimum.y,dz=a.z-item.bounds.minimum.z;
            if(item.seen!=Clock::time_point{} && batch->made-item.seen<=std::chrono::milliseconds(250) && dx*dx+dy*dy+dz*dz<=64.f)
                box.blendFrom=item.bounds;
            item.bounds=box.bounds;item.seen=batch->made;
            batch->boxes[batch->count++]=box;++i;
        }
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){batch->count=0;return false;}
}
// Borion-style local Player proxy. All mutable native access is confined to
// LevelTick/world teardown callbacks; the network middleware reads atomics only.
struct BorionState {
    bool saved{};
    void* actor{};
    std::uint64_t epoch{},runtime{};
    void* registry{};std::uint32_t entity{};
    Vec oldPosition{},oldPrevious{},oldVelocity{},position{};
    bedrock::ActorBounds oldBounds{};
    float oldFall{};
};
BorionState borion;
bedrock::native_motion::Layout motionLayout;
bedrock::CameraRig cameraRig;
std::mutex borionMutex;
std::atomic<bool> suppressMovement{},packetReady{};
std::atomic<void*> observedSender{};
std::atomic<unsigned> movementPacketsSeen{},blockedPackets{};
BDS_HookToken packetToken{},wireToken{};
void* subscribedPacketTarget{};
std::atomic<void*> capturedSender{},capturedSendTarget{};
std::atomic<std::uint64_t> capturedSendEpoch{};
std::size_t statePositionOffset{},statePreviousOffset{},stateVelocityOffset{},fallOffset{};
bedrock::NativePacketSender senderLayout;
bool renderReady{};
std::uint32_t fallHash{};
std::uintptr_t moduleTextBegin{},moduleTextEnd{};

bool inText(void* pointer) noexcept {
    const auto p=reinterpret_cast<std::uintptr_t>(pointer);return p>=moduleTextBegin&&p+16>=p&&p+16<moduleTextEnd;
}

bedrock::PacketWireEvents packetEvents;
bedrock::MovementPackets movementPackets;
struct PacketPose {Vec position{};Clock::time_point seen{};std::uint64_t runtime{};std::uint8_t onGround{};};
struct MovementAnchor {std::uint64_t epoch{},runtime{};std::array<PacketPose,2> poses{};};
// Serialize only scalar packet edits and activation/restoration phases. Never
// hold this across the native send continuation or another network operation.
std::mutex packetMotionMutex,packetPoseMutex;
MovementAnchor observedPacketPoses;
std::atomic<std::shared_ptr<const MovementAnchor>> movementAnchor;
std::atomic<unsigned> anchoredPackets{},interactionPackets{};
std::atomic<unsigned> wirePacketsSeen{},wireMovementRejected{},wireWithoutSender{};

void observeMotionPose(const bedrock::WirePacket& packet,bool auth) noexcept {
    bedrock::MovementPacketPose value;
    if(!movementPackets.pose(packet,value)){wireMovementRejected.fetch_add(1);return;}
    PacketPose pose{value.position,Clock::now(),value.runtimeId,value.onGround};
    const auto e=epoch.load();
    std::unique_lock lock(packetPoseMutex,std::try_to_lock);if(!lock)return;
    if(observedPacketPoses.epoch!=e){observedPacketPoses={};observedPacketPoses.epoch=e;}
    observedPacketPoses.poses[auth?0:1]=pose;
    movementPacketsSeen.fetch_or(auth?2u:1u);
}
bool prepareMovementAnchor(std::uint64_t runtime,Vec position) {
    MovementAnchor anchor;
    {std::unique_lock lock(packetPoseMutex,std::try_to_lock);if(!lock)return false;anchor=observedPacketPoses;}
    if(anchor.epoch!=epoch.load())return false;
    bool ready=false;const auto now=Clock::now();
    for(std::size_t i=0;i<anchor.poses.size();++i){auto& pose=anchor.poses[i];
        const float dx=pose.position.x-position.x,dy=pose.position.y-position.y,dz=pose.position.z-position.z;
        if(pose.seen==Clock::time_point{}||now-pose.seen>std::chrono::milliseconds(500)||
           dx*dx+dy*dy+dz*dz>16.f||(i==1&&pose.runtime!=runtime)){pose={};continue;}
        ready=true;
    }
    if(!ready)return false;
    anchor.runtime=runtime;movementAnchor.store(std::make_shared<const MovementAnchor>(anchor));return true;
}
void packetEvent(bedrock::PacketWireSendEvent& event,const std::shared_ptr<const MovementAnchor>& enteringAnchor) noexcept {
    auto& packet=event.packet();
    if(!bedrock::MovementPackets::handles(event.id()))return;
    const bool auth=event.id()==bedrock::PacketId::PlayerAuthInput;
    std::unique_lock phase(packetMotionMutex);
    const auto anchor=enteringAnchor?enteringAnchor:movementAnchor.load();
    if(!anchor){observeMotionPose(packet,auth);return;}
    const auto& saved=anchor->poses[auth?0:1];
    if(anchor->epoch!=epoch.load()||saved.seen==Clock::time_point{}) {
        disable(ReadFailed);blockedPackets.fetch_add(1);event.cancel();return;
    }
    const auto result=movementPackets.anchor(packet,{saved.position,saved.runtime,saved.onGround},anchor->runtime);
    if(!result.applied){disable(ReadFailed);blockedPackets.fetch_add(1);event.cancel();return;}
    if(result.interaction)interactionPackets.fetch_add(1);
    anchoredPackets.fetch_add(1);
}
void BDS_HOOK_CALL packetSubscriber(void*,void* raw,BDS_HookNext next,void* frame) noexcept {
    // Runtime 4.6 attributes synchronous wire events to this native sender.
    next(frame,raw);
}
void BDS_HOOK_CALL wireSubscriber(void*,void* raw,BDS_HookNext next,void* frame) noexcept {
    auto& args=*static_cast<BDS_PacketWireArgs*>(raw);
    if(!args.sender){wireWithoutSender.fetch_add(1);next(frame,raw);return;}
    if(args.sender!=observedSender.load()){next(frame,raw);return;}
    wirePacketsSeen.fetch_add(1);
    const auto enteringAnchor=movementAnchor.load();
    if(!packetEvents.dispatch(args,[&](bedrock::PacketWireSendEvent& event){packetEvent(event,enteringAnchor);},next,frame)){
        if(suppressMovement.load()){disable(ReadFailed);blockedPackets.fetch_add(1);}else next(frame,raw);
    }
}
void observePacketSender(void* instance) noexcept {
    if(stop.load())return;
    void* sender{};void* target{};
    if(!senderLayout.resolve(instance,sender,target)||!inText(target))return;
    if(packetReady.load() && sender!=observedSender.load()) {
        disable();packetReady.store(false);movementPacketsSeen.store(0);
    }
    capturedSender.store(sender);capturedSendTarget.store(target);capturedSendEpoch.store(epoch.load());
}
bool installPacketSource() {
    if(packetReady.load())return true;
    const auto capturedEpoch=capturedSendEpoch.load();
    void* sender=capturedSender.load();void* target=capturedSendTarget.load();
    if(capturedEpoch!=epoch.load()||!sender||!inText(target))return false;
    if(!BDS_HookOwnsTarget(BDS_HOOK_PACKET_SEND_TO_SERVER,target)) {
        DWORD64 image{};
        const auto* entry=RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(target),&image,nullptr);
        if(!entry||image+entry->BeginAddress!=reinterpret_cast<DWORD64>(target))return false;
    }
    if(!wireToken&&!packetEvents.subscribe(wireSubscriber,nullptr,&wireToken))return false;
    if(packetToken) {
        if(target!=subscribedPacketTarget)return false;
        observedSender.store(sender);packetReady.store(capturedEpoch==epoch.load());return packetReady.load();
    }
    observedSender.store(sender);
    if(!BDS_HookSubscribe(BDS_HOOK_PACKET_SEND_TO_SERVER,target,packetSubscriber,nullptr,&packetToken))return false;
    subscribedPacketTarget=target;packetReady.store(capturedEpoch==epoch.load());
    log("Freecam wire PacketHook ready [Packet ID, pre-batch read/write]; sender RVA="+std::to_string(reinterpret_cast<std::uintptr_t>(target)-reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)))+" [runtime-derived vtable slot; candidate]");
    return true;
}
std::uint64_t actorRuntime(void* actor) noexcept {
    std::uint64_t id{};read(component(actor,actors.runtimeIdComponentHash),actors.runtimeIdValueOffset,id);return id;
}
bool restoreBorionLocked() noexcept {
    std::lock_guard phase(packetMotionMutex);
    if(!borion.saved)return true;
    void* registry{};std::uint32_t entity{};
    if(borion.epoch!=epoch.load()||borion.actor!=local.load()||actorRuntime(borion.actor)!=borion.runtime||
       !read(borion.actor,actors.actorEntityContextOffset+actors.contextRegistryOffset,registry)||registry!=borion.registry||
       !read(borion.actor,actors.actorEntityContextOffset+actors.contextEntityIdOffset,entity)||entity!=borion.entity) {
        cameraRig.reset();borion.saved=false;suppressMovement.store(false);movementAnchor.store(nullptr);active.store(false);return true;
    }
    auto* state=component(borion.actor,actors.stateVectorComponentHash);
    auto* shape=component(borion.actor,actors.aabbShapeComponentHash);
    auto* fall=component(borion.actor,fallHash);
    if(!state||!shape||!fall)return false;
    // Position restore is a teleport, not a frame-long interpolation across
    // the flight path. Borion also clears motion/fall distance when returning.
    const bedrock::native_motion::Snapshot restored{borion.oldPosition,borion.oldPosition,Vec{},borion.oldBounds,0.f};
    const bool ok=bedrock::native_motion::apply(borion.actor,actors,motionLayout,component,restored);
    if(ok){cameraRig.reset();borion.saved=false;suppressMovement.store(false);movementAnchor.store(nullptr);active.store(false);}
    return ok;
}
void restoreBorionBeforeBoundary() noexcept {
    std::unique_lock guard(borionMutex,std::try_to_lock);
    if(guard.owns_lock())restoreBorionLocked();
}
void borionTick(const bedrock::LevelTickEvent&) {
    std::unique_lock guard(borionMutex,std::try_to_lock);if(!guard.owns_lock())return;
    observePacketSender(client.load());
    const auto c=config.load();
    auto cameraScope=nativeCamera.tick(client.load(),bedrock::WorldEpoch{epoch.load()});
    void* player=cameraScope ? nativeCamera.localActor() : nullptr;local.store(player);
    auto typeScope=gameTypes.tick(client.load(),bedrock::WorldEpoch{epoch.load()});
    if((c->spectatorMode||terrainLeaseActive.load())&&gameplay.load()&&player){
        if(const auto type=gameTypes.gameType())
            nativeModeContext.store((epoch.load()<<8)|static_cast<std::uint64_t>(static_cast<int>(*type)+1));
    }
    if(c->visibilityDiagnostics&&!stop.load()){
        static Clock::time_point next{};const auto now=Clock::now();if(now>=next){next=now+std::chrono::milliseconds(500);
            int type=-1;if(gameplay.load()&&player){if(const auto current=gameTypes.gameType())type=static_cast<int>(*current);}
            diagnosticGameType.store(type);diagnosticGameTime.store(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());diagnosticGameEpoch.store(epoch.load());
        }
    }
    if(borion.saved && (restoreRequested.load()||stop.load()||!desired.load()||!gameplay.load()||!c->enabled||
       player!=borion.actor||borion.epoch!=epoch.load())) {
        if(!restoreBorionLocked())note(ReadFailed);else restoreRequested.store(false);return;
    }
    if(!borion.saved)restoreRequested.store(false);
    if(!stop.load()&&gameplay.load()) {
        static Clock::time_point next{};
        if(Clock::now()>=next) {
            next=Clock::now()+std::chrono::seconds(5);
            const auto sample=nativeCamera.readPlayerProxy();
            if(sample)log(nlohmann::json({{"event","freecam-player-pose"},{"runtime_id",sample->runtimeId},
                {"position",{sample->position.x,sample->position.y,sample->position.z}},
                {"velocity",{sample->velocity.x,sample->velocity.y,sample->velocity.z}},
                {"fall_distance",sample->fallDistance},{"sender_ready",packetReady.load()},
                {"movement_packet_kinds",movementPacketsSeen.load()},{"wire_packets",wirePacketsSeen.load()},
                {"wire_movement_rejected",wireMovementRejected.load()},{"wire_without_sender",wireWithoutSender.load()},
                {"wire_bridge_runtime_verified",false}}).dump());
            else log("Freecam waiting: local Actor / StateVector / AABB / rotation / FallDistance snapshot unavailable");
        }
    }
    if(stop.load()||!desired.load()||!gameplay.load()||!c->enabled||!player)return;
    if(!packetReady.load()||!movementPacketsSeen.load()){note(Waiting);return;}
    const auto pose=nativeCamera.readPlayerProxy();int hurt{};
    if(!cameraScope||!pose||(c->disableOnDamage&&!hurtTime(player,hurt))){disable(ReadFailed);restoreBorionLocked();return;}
    const auto position=pose->position,previous=pose->previous,velocity=pose->velocity;
    const auto bounds=pose->bounds;const float fallDistance=pose->fallDistance,yaw=pose->rotation.yaw;
    if(c->disableOnDamage&&hurt>0){disable(Damage);restoreBorionLocked();return;}
    if(!borion.saved) {
        const auto runtime=actorRuntime(player);if(!runtime||!bounds.valid()) {disable(ReadFailed);return;}
        void* registry{};std::uint32_t entity{};
        if(!read(player,actors.actorEntityContextOffset+actors.contextRegistryOffset,registry)||
           !read(player,actors.actorEntityContextOffset+actors.contextEntityIdOffset,entity)){disable(ReadFailed);return;}
        std::unique_lock phase(packetMotionMutex);
        if(!prepareMovementAnchor(runtime,position)){note(Waiting);return;}
        borion={true,player,epoch.load(),runtime,registry,entity,position,previous,velocity,position,bounds,fallDistance};
        if(!cameraRig.begin(position,bounds,runtime,borion.epoch)){disable(ReadFailed);restoreBorionLocked();return;}
        suppressMovement.store(true);flightSerial.fetch_add(1);active.store(true);note(Activated);
    }
    if(actorRuntime(player)!=borion.runtime){disable(ReadFailed);restoreBorionLocked();return;}
    const auto down=[](unsigned key){return keys[key].load()?1.f:0.f;};
    const float forward=down(c->forward)-down(c->back),side=down(c->right)-down(c->left);
    const float speed=c->speed/20.f*(keys[c->fast].load()?c->fastMultiplier:1.f);
    if(c->fullBright)brightness.fullBright();else brightness.reset();
    const auto cameraFrame=cameraRig.advance(bedrock::Yaw{yaw},{forward,side,down(c->up)-down(c->down)},
        {speed*20.f,10.f},std::chrono::duration<float>{.05f},borion.runtime,epoch.load());
    if(!cameraFrame){disable(ReadFailed);restoreBorionLocked();return;}
    borion.position=cameraFrame->position;
    if(!finite(borion.position)){disable(ReadFailed);restoreBorionLocked();return;}
    if(nativeCamera.applyPlayerProxy(*cameraFrame,borion.runtime,bedrock::WorldEpoch{epoch.load()})!=bedrock::CameraChange::Applied) {
        disable(ReadFailed);restoreBorionLocked();return;
    }
    if(renderReady&&c->playerHitboxes){Capture batch;batch.epoch=epoch.load();batch.origin=borion.position;
        if(capturePlayers(player,&batch,c->captureRows)) {std::unique_lock lock(mailboxMutex,std::try_to_lock);
            if(lock.owns_lock()){mailbox=batch;mailboxReady=true;}}
    }
}

using BeginFn=void(__fastcall*)(void*,int,int,int,bool);
using VertexFn=void(__fastcall*)(void*,float,float,float);
struct Color {float r,g,b,a;};
using MeshFn=void(__fastcall*)(void*,void*,void*,void*);
using NameCtor=void*(__fastcall*)(void*,const char*);
using MaterialCtor=void*(__fastcall*)(void*,void*,const void*);
BeginFn begin{};VertexFn vertex{};MeshFn submit{};NameCtor nameCtor{};MaterialCtor materialCtor{};
void* materialGroup{};alignas(16) std::array<std::byte,48> materialName{};
alignas(16) std::array<std::byte,16> material{};int materialState{};
// All native drawing stays on the original render thread. Meshes contain only
// copied floats and are prepared by one worker; no native pointers reach it.
bool drawNative(void* renderer,void* screen,const Mesh* geometry) noexcept {
    void* player{};void* tess{};void* shader{};Vec origin{},oldTranslation{};Color oldColor{};
    std::uint8_t busy{},special{};const std::uint8_t dirty=1;const Color color{0.3f,0.95f,0.5f,1.f};
    if(!read(renderer,render.player,player)||!read(player,render.origin,origin)||!finite(origin)||
       !read(screen,render.tess,tess)||!read(screen,render.shader,shader)||
       !read(tess,render.building,busy)||busy||!read(tess,render.special,special)||special||
       !read(tess,render.translation,oldTranslation)||!read(shader,0,oldColor))return true;
    bool success=false;
    __try {
        if(!materialState){
            nameCtor(materialName.data(),"name_tag");materialCtor(material.data(),materialGroup,materialName.data());
            void* info{};if(!read(material.data(),0,info)||!info)materialState=-1;else materialState=1;
        }
        if(materialState>0&&write(tess,render.translation,Vec{})&&write(shader,0,color)&&write(shader,render.dirty,dirty)){
            begin(tess,0,4,static_cast<int>(geometry->count),false);
            const float alpha=std::clamp(std::chrono::duration<float>(Clock::now()-geometry->made).count()/0.05f,0.f,1.f);
            for(unsigned i=0;i<geometry->count;++i){
                const auto a=geometry->fromVertices[i],b=geometry->vertices[i];
                vertex(tess,a.x+(b.x-a.x)*alpha-origin.x,a.y+(b.y-a.y)*alpha-origin.y,a.z+(b.z-a.z)*alpha-origin.z);
            }
            alignas(16) std::array<std::byte,0x58> scratch{};submit(screen,tess,material.data(),scratch.data());success=true;
        }
    }__except(EXCEPTION_EXECUTE_HANDLER){success=false;}
    write(shader,0,oldColor);write(shader,render.dirty,dirty);write(tess,render.translation,oldTranslation);
    return success;
}
void BDS_HOOK_CALL renderSubscriber(void*,void* raw,BDS_HookNext next,void* frame){
    auto& args=*static_cast<BDS_RenderLevelArgs*>(raw);
    void* camera{};if(read(args.renderer,render.player,camera)&&camera){primaryCamera.store(camera);primaryCameraEpoch.store(epoch.load());}
    updateTerrainCulling(args.renderer);
    if(!cameraRenderReady.load())captureVisibility(args.renderer);
    next(frame,raw);if(!active.load()||stop.load()||!config.load()->playerHitboxes)return;
    static thread_local bool drawing=false;if(drawing)return;
    const auto geometry=mesh.load();if(!geometry||geometry->epoch!=epoch.load()||!geometry->count||Clock::now()-geometry->made>std::chrono::milliseconds(250))return;
    drawing=true;
    if(!drawNative(args.renderer,args.screen,geometry.get())){disable(RenderFault);}
    drawing=false;
}
std::uint32_t number(const nlohmann::json& j){
    const auto n=j.is_string()?std::stoull(j.get<std::string>(),nullptr,0):j.get<std::uint64_t>();
    if(n>UINT32_MAX)throw std::runtime_error("profile integer overflow");return static_cast<std::uint32_t>(n);
}
void* checkedFunction(const nlohmann::json& j){
    const auto rva=number(j.at("rva"));const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto text=j.at("entry_check").get<std::string>();if(!rva||text.size()<23||text.size()>191||(text.size()+1)%3)throw std::runtime_error("bad native guard");
    const auto ptr=reinterpret_cast<const std::byte*>(base+rva);MEMORY_BASIC_INFORMATION mbi{};
    if(!VirtualQuery(ptr,&mbi,sizeof(mbi))||mbi.State!=MEM_COMMIT||!(mbi.Protect&(PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE)))throw std::runtime_error("native target not executable");
    for(std::size_t i=0;i<text.size();i+=3){std::uint8_t value{};const auto expected=std::stoul(text.substr(i,2),nullptr,16);
        if(!read(ptr,i/3,value)||value!=expected)throw std::runtime_error("native entry guard mismatch at RVA="+std::to_string(rva)+" byte="+std::to_string(i/3)+" expected="+std::to_string(expected)+" observed="+std::to_string(value));}
    return const_cast<std::byte*>(ptr);
}
void prepareProfile(const std::string& profile){
    const auto j=bedrock::parseNativeProfile(profile);const auto& f=j.at("native_camera");
    if(f.at("schema")!=1||f.at("pointer_size")!=8)throw std::runtime_error("unsupported Freecam layout");
    hurtHash=number(f.at("hurt_time_component_hash"));hurtOffset=number(f.at("hurt_time_value_offset"));
    if(hurtOffset>4096||f.at("hurt_time_value_size")!=4)throw std::runtime_error("invalid camera/hurt layout");
    const auto& visual=f.at("visual");
    if(j.at("target").value("version",std::string{})=="1.26.60.5"&&visual.value("revision",0)<10)
        throw std::runtime_error("Freecam visual definition predates InsideBlockEffect port; exact embedded profile required");
    if(visual.at("schema")!=1)throw std::runtime_error("unsupported camera visual schema");
    motionLayout=bedrock::native_motion::Layout::fromProfile(profile);
    statePositionOffset=number(j.at("native_motion").at("state_vector").at("state_position_offset"));statePreviousOffset=motionLayout.previous;
    stateVelocityOffset=motionLayout.velocity;fallHash=motionLayout.fallHash;fallOffset=motionLayout.fall;
    senderLayout.load(j);packetEvents.load(j);
    if(statePositionOffset!=0||statePreviousOffset!=12||stateVelocityOffset!=24||fallOffset!=0)
        throw std::runtime_error("unsupported Borion native actor layout");

    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    moduleTextBegin=base+number(j.at("target").at("text_rva"));moduleTextEnd=moduleTextBegin+number(j.at("target").at("text_size"));
    std::string error;if(!bedrock::parseNativeActorLayout(profile,actors,error))
        throw std::runtime_error(error);
    renderReady=bedrock::parseWorldRenderProfile(profile,render,error);
}
std::filesystem::path directory(){
    PWSTR path{};if(FAILED(SHGetKnownFolderPath(FOLDERID_Documents,KF_FLAG_CREATE,nullptr,&path)))throw std::runtime_error("Documents folder unavailable");
    std::filesystem::path result(path);CoTaskMemFree(path);return result/L"PEXData"/L"Freecam";
}
bool loadConfig(const std::filesystem::path& path){
    try{std::ifstream in(path);if(!in)throw std::runtime_error("cannot open config");const std::string text((std::istreambuf_iterator<char>(in)),{});
        auto json=nlohmann::json::parse(text);
        bool rewrite=false;
        if(!json.contains("config_revision")) {
            if(json.value("toggle_key",86)==86) json["toggle_key"]=77;
            json["config_revision"]=2;rewrite=true;
        }
        if(!json.contains("spectator_mode")){json["spectator_mode"]=false;rewrite=true;}
        if(!json.contains("visibility_diagnostics")){json["visibility_diagnostics"]=false;rewrite=true;}
        if(!json.contains("full_bright")){json["full_bright"]=true;rewrite=true;}
        const auto next=std::make_shared<const freecam::Config>(freecam::parseConfig(json.dump()));
        if(rewrite){std::ofstream migrated(path,std::ios::trunc);migrated<<json.dump(2)<<'\n';
            if(!migrated)throw std::runtime_error("config update failed");}
        // Key binding changes cannot leave old movement keys held in the game.
        disable();config.store(next);for(auto& key:keys)key.store(false);log("config ready");return true;
    }catch(const std::exception& e){log(std::string("config rejected: ")+e.what());return false;}
}
void worker(const std::filesystem::path& path){
    std::ofstream observations;

    std::uint64_t generation{};auto lastReload=Clock::now();Clock::time_point nextWaitingLog{},nextPacketLog{};std::error_code ec;auto modified=std::filesystem::last_write_time(path,ec);
    constexpr std::array<std::array<unsigned,2>,12> edges{{{0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7}}};
    while(!stop.load()){
        const auto now=Clock::now();
        VisibilitySample sample;bool observed=false;
        {std::unique_lock lock(visibilityMutex,std::try_to_lock);if(lock&&visibilityReady){sample=visibilityMailbox;visibilityReady=false;observed=true;}}
        if(config.load()->visibilityDiagnostics&&observed){
            if(!observations.is_open())observations.open(path.parent_path()/L"visibility.jsonl",std::ios::app);
            if(observations){auto vector=[](Vec v){return nlohmann::json::array({v.x,v.y,v.z});};
                observations<<nlohmann::json{{"schema",1},{"build",profileGameVersion},{"visual_revision",visualRevision},
                    {"exe_sha256",profileExeSha},
                    {"monotonic_ms",sample.milliseconds},{"epoch",sample.epoch},{"freecam",sample.flying},{"actor_game_type",sample.gameType},
                    {"valid_camera_read",sample.valid},{"candidate_camera_654",vector(sample.camera654)},
                    {"render_origin",vector(sample.origin)},{"raw_camera_970_976",sample.fogFlags},{"culler_type",sample.cullerType},
                    {"culler_vtable_rva",sample.cullerVtable},{"camera_view_flags_valid",sample.viewValid},
                    {"raw_view_flags_7c_88",sample.viewFlags},{"draw_inside_cubes",sample.viewValid?nlohmann::json(bool(sample.viewFlags[4])):nlohmann::json(nullptr)}}.dump()<<'\n';observations.flush();}
        }else if(!config.load()->visibilityDiagnostics&&observations.is_open())observations.close();
        if(!packetReady.load())installPacketSource();
        const auto e=epoch.load();if(generation!=e){generation=e;mesh.store(nullptr);}
        Capture batch;bool have=false;{std::lock_guard lock(mailboxMutex);if(mailboxReady){batch=mailbox;mailboxReady=false;have=true;}}
        if(have&&batch.epoch==generation&&active.load()){
            const auto c=config.load();auto result=std::make_shared<Mesh>();result->epoch=generation;result->made=batch.made;unsigned boxes=0;
            for(unsigned i=0;i<batch.count&&boxes<c->maxPlayers;++i){
                const auto& box=batch.boxes[i];const auto& b=box.bounds;const auto& from=box.blendFrom;
                const float x=(b.minimum.x+b.maximum.x)*0.5f-batch.origin.x,
                    y=(b.minimum.y+b.maximum.y)*0.5f-batch.origin.y,z=(b.minimum.z+b.maximum.z)*0.5f-batch.origin.z;
                if(x*x+y*y+z*z>c->radius*c->radius)continue;
                for(const auto& edge:edges)for(unsigned corner:edge){
                    result->fromVertices[result->count]={(corner&1)?from.maximum.x:from.minimum.x,(corner&2)?from.maximum.y:from.minimum.y,(corner&4)?from.maximum.z:from.minimum.z};
                    result->vertices[result->count++]={(corner&1)?b.maximum.x:b.minimum.x,(corner&2)?b.maximum.y:b.minimum.y,(corner&4)?b.maximum.z:b.minimum.z};
                }++boxes;
            }
            if(active.load()&&generation==epoch.load())mesh.store(result);
        }
        if(!active.load())mesh.store(nullptr);
        if((active.load()||interactionPackets.load()||blockedPackets.load()||terrainPending.load()||terrainRestores.load()||terrainFaults.load())&&now>=nextPacketLog){nextPacketLog=now+std::chrono::seconds(5);
            log("Freecam spectator culler: ready="+std::to_string(terrainReady.load())+" mode="+std::to_string(terrainObservedMode.load())+
                " pending="+std::to_string(terrainPending.load())+" calls="+std::to_string(terrainCalls.exchange(0))+
                " changed="+std::to_string(terrainChanges.exchange(0))+" restored="+std::to_string(terrainRestores.exchange(0))+
                " faults="+std::to_string(terrainFaults.exchange(0))+" native_requests="+std::to_string(selectorRequests.exchange(0))+
                " rewritten="+std::to_string(selectorRewrites.exchange(0))+" caller_rva="+std::to_string(selectorCaller.load())+
                " churn_guard="+std::to_string(terrainRejected.load())+" [native selector mode5; underground view needs live confirmation]");
            log("Freecam view factory: ready="+std::to_string(createViewReady.load())+" created="+std::to_string(viewCreated.exchange(0))+
                " changed="+std::to_string(viewChanged.exchange(0))+" failed="+std::to_string(viewWriteFailed.exchange(0))+
                " camera_calls="+std::to_string(cameraRenderCalls.exchange(0))+" [fresh output only; 5s; visual_revision="+std::to_string(visualRevision)+"]");
            log("Freecam RenderDragon inside effect: ready="+std::to_string(effectReady.load())+" calls="+std::to_string(effectCalls.exchange(0))+
                " eligible="+std::to_string(effectEligible.exchange(0))+" skipped="+std::to_string(effectSkipped.exchange(0))+
                " other_caller="+std::to_string(effectOtherCaller.exchange(0))+" caller_rva="+std::to_string(effectLastCaller.load())+" [5s; visual_revision="+std::to_string(visualRevision)+"]");
            log("Freecam packet flow: anchored="+std::to_string(anchoredPackets.exchange(0))+" item_inputs="+std::to_string(interactionPackets.exchange(0))+" rejected="+std::to_string(blockedPackets.exchange(0)));
        }
        const auto bits=logBits.exchange(0);
        if(bits&Activated){log("Freecam active [Borion local Player proxy; packet positions anchored; interactions forwarded; Player boxes: every tick + 50ms interpolation]");
            const auto c=config.load();log(std::string("Freecam view: spectator_mode=")+(c->spectatorMode?"on":"off")+
                " spectator_culler="+(terrainReady.load()?"mode5 ready":"unavailable")+" visual_revision="+std::to_string(visualRevision)+" full_bright="+(c->fullBright?(gammaReady.load()?"gamma 25":"unavailable"):"off"));
        }
        if(bits&Waiting && now>=nextWaitingLog){log("Freecam waiting: sender hook / fresh exact-build movement packet pose required before native writes");nextWaitingLog=now+std::chrono::seconds(5);}
        if(bits&Disabled)log("Freecam disabled");if(bits&Damage)log("Freecam disabled: local Player hurt");
        if(bits&ReadFailed)log("Freecam disabled: native Player / movement packet validation failed");
        if(bits&World)log("world changed: Freecam and Player boxes reset");if(bits&RenderFault)log("Freecam disabled: native render fault");
        if(now-lastReload>std::chrono::milliseconds(500)){
            lastReload=now;const auto current=std::filesystem::last_write_time(path,ec);
            if(!ec&&current!=modified){modified=current;loadConfig(path);}
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
DWORD WINAPI start(void*){
    HMODULE pinned{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&start),&pinned);
    status.store(1);
    try{
        const auto dir=directory();std::filesystem::create_directories(dir);logFile.open(dir/L"Freecam.log",std::ios::trunc);
        const auto path=dir/L"config.json";if(!std::filesystem::exists(path)){std::ofstream out(path);out<<freecam::configJson(freecam::Config{}).dump(2)<<'\n';}
        if(!loadConfig(path))throw std::runtime_error("initial config invalid");
        std::string error;bedrock::ProfileManager profiles;
        const bedrock::ProfileRequirements required{bedrock::Feature::Input,bedrock::Feature::Gameplay,bedrock::Feature::LevelTick,bedrock::Feature::Camera,bedrock::Feature::Motion,
            bedrock::Feature::PacketSender,bedrock::Feature::PacketWire,bedrock::Feature::WorldEvents};
        const bedrock::ProfileOptions options{bedrock::UpdatePolicy::RefreshIfNeeded,bedrock::CandidatePolicy::AllowCandidates};
        if(!profiles.prepare(required,options,[](const bedrock::RuntimeProfile& selected,std::string& failure){
            try{prepareProfile(selected.json());if(!nativeCamera.load(selected,failure))return false;
                std::string optional;if(!gameTypes.load(selected,optional))log("Freecam native game type observation unavailable: "+optional);
                return true;}catch(const std::exception& e){failure=e.what();return false;}
        },error))throw std::runtime_error(error);
        const auto selected=profiles.snapshot();const auto& profile=selected->json();const auto& resolver=selected->resolver();
        const auto j=bedrock::parseNativeProfile(profile);const auto& f=j.at("native_camera");
        visualRevision=f.at("visual").value("revision",9);profileGameVersion=selected->choice().version;profileExeSha=j.at("target").at("sha256").get<std::string>();
        log("Freecam profile source="+std::string(bedrock::profileSourceName(selected->choice().source))+
            " version="+selected->choice().version+" path="+selected->choice().path);
        // Include selection/load errors even when an old owner's capabilities
        // stop startup before the normal ready message.
        log(BDS_HookRuntimeDiagnostic());
        if(!BDS_HookRuntimeSupports(BDS_HOOK_PACKET_SEND_TO_SERVER))
            throw std::runtime_error("active runtime lacks packet send capability; restart with Runtime 4.2 DLL");
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));std::array<void*,7> functions{};
        try { if(renderReady) for(unsigned i=0;i<7;++i){const auto& fn=render.functions[i];auto* target=reinterpret_cast<void*>(base+fn.rva);
            if(i==0&&BDS_HookOwnsTarget(BDS_HOOK_RENDER_LEVEL,target)){functions[i]=target;continue;}
            functions[i]=checkedFunction({{"rva",fn.rva},{"entry_check",[&]{std::string s;const char* hex="0123456789abcdef";for(auto b:fn.guard){if(!s.empty())s+=' ';s+=hex[b>>4];s+=hex[b&15];}return s;}()}});
        }
        } catch(const std::exception& e) { renderReady=false;log(std::string("Freecam native drawing unavailable: ")+e.what()); }
        begin=reinterpret_cast<BeginFn>(functions[1]);vertex=reinterpret_cast<VertexFn>(functions[2]);submit=reinterpret_cast<MeshFn>(functions[4]);
        nameCtor=reinterpret_cast<NameCtor>(functions[5]);materialCtor=reinterpret_cast<MaterialCtor>(functions[6]);materialGroup=reinterpret_cast<void*>(base+render.commonMaterialGroup);
        try {
            if(!brightness.load(*selected,error)||!brightness.install(error))throw std::runtime_error(error);
            gammaReady.store(true);
            log("SDK brightness API ready [owner-scoped gamma override]");
        } catch(const std::exception& e){log(std::string("Freecam full brightness unavailable: ")+e.what());}
        try {
            if(f.at("visual").value("spectator_mode_behavior",std::string{})!="native_spectator_culler_mode5")
                throw std::runtime_error("spectator terrain profile is unresolved; basic Freecam remains available");
            if(!renderReady)throw std::runtime_error("exact-build renderer layout unavailable");
            const auto& culling=f.at("visual").at("terrain_culling");
            if(culling.at("schema")!=1||number(culling.at("spectator_mode"))!=spectatorCullerMode)throw std::runtime_error("unsupported terrain culler schema");
            setCuller=reinterpret_cast<SetCullerFn>(checkedFunction(culling.at("set_culler")));
            checkedFunction(culling.at("factory_check"));
            const auto updateSpectator=checkedFunction(culling.at("spectator_update_check"));
            cameraCullerOffset=number(culling.at("camera_culler_offset"));cameraCullerTypeOffset=number(culling.at("camera_culler_type_offset"));
            spectatorCullerVtable=base+number(culling.at("spectator_vtable_rva"));void* updateEntry{};
            if(cameraCullerOffset!=0x9a8||cameraCullerTypeOffset!=0x9b8||!read(reinterpret_cast<void*>(spectatorCullerVtable),8,updateEntry)||updateEntry!=updateSpectator)
                throw std::runtime_error("terrain culler identity mismatch");
            if(!BDS_HookDirectInstall(reinterpret_cast<void*>(setCuller),reinterpret_cast<void*>(&cullerDetour),reinterpret_cast<void**>(&originalSetCuller),&cullerToken))
                throw std::runtime_error("native culler selector hook conflict");
            setCuller=originalSetCuller;terrainReady.store(true);
            log("Freecam spectator culler ready [native selector hook; mode5/vtable0xE8DFE00; game mode unchanged; visual revision9]");
        }catch(const std::exception& e){terrainReady.store(false);log(std::string("Freecam terrain culling unavailable: ")+e.what());}
        try{
            if(!renderReady)throw std::runtime_error("exact-build renderer layout unavailable");
            void* target=checkedFunction(f.at("visual").at("camera_render_observe"));
            cameraRenderReady.store(BDS_HookDirectInstall(target,reinterpret_cast<void*>(&cameraRenderDetour),reinterpret_cast<void**>(&originalCameraRender),&cameraRenderToken)!=0);
            log(cameraRenderReady.load()?"Freecam native ViewRenderData observer ready [read only; no overlay suppression]":"Freecam ViewRenderData observer unavailable: hook conflict");
        }catch(const std::exception& e){log(std::string("Freecam ViewRenderData observer unavailable: ")+e.what());}
        try{
            void* target=checkedFunction(f.at("visual").at("create_view"));
            createViewReady.store(BDS_HookDirectInstall(target,reinterpret_cast<void*>(&createViewDetour),reinterpret_cast<void**>(&originalCreateView),&createViewToken)!=0);
            log(createViewReady.load()?"Freecam view factory ready [new output draw_inside_cubes=false; no game-mode/ability writes; visual revision9]":"Freecam view factory unavailable: hook conflict");
        }catch(const std::exception& e){log(std::string("Freecam view factory unavailable: ")+e.what());}
        try{
            const auto& effect=f.at("visual").at("render_dragon_inside_effect");
            if(effect.at("variants").size()!=2)throw std::runtime_error("unsupported InsideBlockEffect variants");
            void* target=checkedFunction(effect.at("predicate"));
            for(unsigned i=0;i<2;++i){const auto& v=effect.at("variants").at(i);
                checkedFunction(v.at("caller_check"));const auto typeGetter=checkedFunction(v.at("type_getter_check"));
                effectTables[i]=base+number(v.at("callable_vtable_rva"));effectCallers[i]=base+number(v.at("return_rva"));
                void *call{},*type{};
                if(!read(reinterpret_cast<void*>(effectTables[i]),0x10,call)||call!=target||
                   !read(reinterpret_cast<void*>(effectTables[i]),0x18,type)||type!=typeGetter)
                    throw std::runtime_error("InsideBlockEffect callable identity mismatch");
            }
            effectReady.store(BDS_HookDirectInstall(target,reinterpret_cast<void*>(&effectPredicateDetour),reinterpret_cast<void**>(&originalEffectPredicate),&effectToken)!=0);
            log(effectReady.load()?"Freecam RenderDragon InsideBlockEffect filter ready [tag + exact extraction caller; versioned visual profile]":"Freecam RenderDragon InsideBlockEffect filter unavailable: hook conflict");
        }catch(const std::exception& e){log(std::string("Freecam RenderDragon InsideBlockEffect filter unavailable: ")+e.what());}
        log("Freecam legacy inside-cube bypass retired [revision7 did not resolve underground display]");
        api.Event.Enum.Keyboard.then(keyEvent);api.Event.Enum.Mouse.then(mouseEvent);
        api.Event.Enum.Gameplay.then([](const bedrock::GameplayEvent& e){api.setWorldEventClient(e.clientInstance);client.store(e.clientInstance);gameplay.store(e.active);if(!e.active){disable();for(auto& key:keys)key.store(false);}});
        api.Event.Enum.WorldChange.then([](const bedrock::WorldChangeEvent& event){
            if(event.reason==bedrock::WorldChangeReason::Left || event.reason==bedrock::WorldChangeReason::JoinCancelled)
                restoreBorionBeforeBoundary();
            // Post-transition notifications must never restore old coordinates
            // into the new world. The next tick discards the expired epoch.
            reset();packetReady.store(false);observedSender.store(nullptr);movementPacketsSeen.store(0);
            capturedSendEpoch.store(0);
        });
        api.Event.Enum.LevelTick.then(borionTick);
        hooks=std::make_unique<bedrock::GameHooks>(api);
        if(!hooks->installInput(resolver,error)||!hooks->installGameplay(resolver,error)||!hooks->installLevelTick(resolver,error)||!hooks->installWorldEvents(resolver,profile,error))throw std::runtime_error(error);
        if(renderReady && !BDS_HookSubscribe(BDS_HOOK_RENDER_LEVEL,functions[0],renderSubscriber,nullptr,&renderToken))
            throw std::runtime_error("render hook conflict/install failure; restart Minecraft with matching runtime DLLs");
        if(!renderReady)log("Freecam player hitboxes unavailable: exact-build native drawing definitions are pending; flight core remains enabled");
        log("ready "+j.at("target").at("version").get<std::string>()+"; Borion Freecam candidate; press Esc and return to gameplay, then configured toggle key (default M); compatible runtime");log(std::string("active runtime owner: ")+BDS_HookRuntimeVersion());status.store(2);worker(path);
    }catch(const std::exception& e){log(std::string("startup failed: ")+e.what());status.store(4);}
    disable();
    // Keep the render subscriber pinned: a later frame restores an owned culler.
    // Do not tear down trampolines while a native thread may still enter them.
    // Keep pinned callbacks as no-ops/pass-through after stop, so the next
    // owned game tick can restore an outstanding proxy before movement resumes.
    stop.store(true);if(status.load()!=4)status.store(3);return 0;
}
}
extern "C" __declspec(dllexport) unsigned Freecam_GetStatus(){return status.load();}
extern "C" __declspec(dllexport) void Freecam_Stop(){stop.store(true);disable();}
extern "C" __declspec(dllexport) bool Freecam_IsActive(){return active.load();}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){DisableThreadLibraryCalls(module);const auto thread=CreateThread(nullptr,0,start,nullptr,0,nullptr);if(thread)CloseHandle(thread);else status.store(4);}
    else if(reason==DLL_PROCESS_DETACH)stop.store(true);return TRUE;
}
#endif
