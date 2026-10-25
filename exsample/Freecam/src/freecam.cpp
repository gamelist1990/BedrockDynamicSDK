#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <bedrock/game_api.hpp>
#include <bedrock/game_hooks.hpp>
#include <bedrock/generated_symbols.hpp>
#include <bedrock/native_actors.hpp>
#include <bedrock/native_items.hpp>
#include <bedrock/native_world_render.hpp>
#include <bedrock/remote_catalog.hpp>
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
std::atomic<void*> client{},local{};
std::atomic<std::uint64_t> epoch{1};
std::array<std::atomic<bool>,256> keys{};
std::atomic<std::shared_ptr<const freecam::Config>> config{std::make_shared<const freecam::Config>()};
bedrock::GameApi api;std::unique_ptr<bedrock::GameHooks> hooks;
bedrock::NativeActorLayout actors;bedrock::NativeItemLayout items;bedrock::WorldRenderProfile render;
std::size_t hurtOffset{};std::uint32_t hurtHash{};
BDS_HookToken localToken{},renderToken{};
using LocalFn=void*(__fastcall*)(void*);LocalFn originalLocal{};
using GammaFn=float(__fastcall*)(void*);GammaFn originalGamma{};
BDS_HookToken gammaToken{};
// View-only camera predicate. Never changes ActorGameTypeComponent.
using InsideBlockFn=bool(__fastcall*)(void*);
InsideBlockFn originalInsideBlock{};BDS_HookToken insideBlockToken{};
std::atomic<bool> insideBlockReady{};
std::atomic<unsigned> insideBlockSuppressed{};
bool __fastcall insideBlockDetour(void* instance){
    if(active.load()&&!stop.load()&&instance==client.load()&&config.load()->spectatorMode){
        insideBlockSuppressed.fetch_add(1,std::memory_order_relaxed);return false;
    }
    return originalInsideBlock(instance);
}
std::atomic<bool> gammaReady{};
float __fastcall gammaDetour(void* options){
    const float value=originalGamma(options);
    return active.load()&&!stop.load()&&config.load()->fullBright?25.f:value;
}
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
void disable(unsigned reason=Disabled){restoreRequested.store(true);desired.store(false);active.store(false);mesh.store(nullptr);note(reason);}
void reset(){disable(World);epoch.fetch_add(1);local.store(nullptr);for(auto& key:keys)key.store(false);}
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
void* __fastcall localDetour(void* instance){void* player=originalLocal(instance);client.store(instance);local.store(player);observePacketSender(instance);return player;}
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
std::mutex borionMutex;
std::atomic<bool> suppressMovement{},packetReady{};
std::atomic<void*> observedSender{};
std::atomic<unsigned> movementPacketsSeen{},blockedPackets{};
BDS_HookToken packetToken{};
void* subscribedPacketTarget{};
std::atomic<void*> capturedSender{},capturedSendTarget{};
std::atomic<std::uint64_t> capturedSendEpoch{};
std::size_t statePositionOffset{},statePreviousOffset{},stateVelocityOffset{},fallOffset{},senderOffset{},sendSlot{},packetIdSlot{};
std::uint32_t fallHash{};
std::uintptr_t moduleTextBegin{},moduleTextEnd{};

bool inText(void* pointer) noexcept {
    const auto p=reinterpret_cast<std::uintptr_t>(pointer);return p>=moduleTextBegin&&p+16>=p&&p+16<moduleTextEnd;
}

struct MotionPacketLayout {
    std::uintptr_t vtable{};
    std::size_t position{},delta{},analog{},move{},rawMove{},flags{},vehicle{};
};
MotionPacketLayout authPacket,movePacket;
struct PacketPose {Vec position{};Clock::time_point seen{};std::uint64_t runtime{};std::uint8_t onGround{};};
struct MovementAnchor {std::uint64_t epoch{},runtime{};std::array<PacketPose,2> poses{};};
// Serialize only scalar packet edits and activation/restoration phases. Never
// hold this across the native send continuation or another network operation.
std::mutex packetMotionMutex,packetPoseMutex;
MovementAnchor observedPacketPoses;
std::atomic<std::shared_ptr<const MovementAnchor>> movementAnchor;
std::atomic<unsigned> anchoredPackets{},interactionPackets{};

bool packetMatches(void* packet,const MotionPacketLayout& layout) noexcept {
    void* table{};return read(packet,0,table)&&reinterpret_cast<std::uintptr_t>(table)==layout.vtable;
}
void observeMotionPose(void* packet,bool auth) noexcept {
    PacketPose pose;const auto& layout=auth?authPacket:movePacket;
    if(!read(packet,layout.position,pose.position)||!finite(pose.position))return;
    std::uint64_t vehicle{};
    if(!read(packet,layout.vehicle,vehicle)||vehicle!=(auth?UINT64_MAX:0))return;
    if(!auth){std::uint8_t mode{};
        if(!read(packet,0x30,pose.runtime)||!read(packet,0x50,mode)||mode!=0||
           !read(packet,0x51,pose.onGround)||pose.onGround>1)return;
    }
    pose.seen=Clock::now();const auto e=epoch.load();
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
bool anchorMotionPacket(void* packet,bool auth,const MovementAnchor& anchor) noexcept {
    const auto& layout=auth?authPacket:movePacket;const auto& pose=anchor.poses[auth?0:1];
    if(anchor.epoch!=epoch.load()||pose.seen==Clock::time_point{})return false;
    Vec position{};if(!read(packet,layout.position,position)||!finite(position))return false;
    if(!auth){std::uint64_t runtime{},vehicle{};std::uint8_t mode{};
        if(!read(packet,0x30,runtime)||runtime!=anchor.runtime||!read(packet,layout.vehicle,vehicle)||vehicle||
           !read(packet,0x50,mode)||mode!=0)return false;
        return write(packet,layout.position,pose.position)&&write(packet,0x51,pose.onGround);
    }
    std::array<std::uint64_t,2> flags{};std::uint64_t vehicle{};
    if(!read(packet,layout.flags,flags)||!read(packet,layout.vehicle,vehicle)||vehicle!=UINT64_MAX)return false;
    // Keep tick, acknowledgements, item/block actions and their owning payloads.
    // Strip only movement controls. Flags 34/35/36 must remain paired with the
    // transaction/request/action data checked by the game's isValid method.
    std::uint64_t controls=(std::uint64_t{1}<<34)-1;
    for(unsigned bit:{40u,41u,42u,43u,45u,46u,47u,51u,52u,54u,55u,56u,57u,59u,60u,61u,62u,63u})controls|=std::uint64_t{1}<<bit;
    const bool interaction=(flags[0]&((std::uint64_t{7}<<34)|(std::uint64_t{1}<<53)))!=0;
    flags[0]&=~controls;flags[1]&=~std::uint64_t{1}; // SneakCurrentRaw is bit 64.
    const std::array<float,2> zero{};
    if(!write(packet,layout.position,pose.position)||!write(packet,layout.delta,Vec{})||
       !write(packet,layout.analog,zero)||!write(packet,layout.move,zero)||
       !write(packet,layout.rawMove,zero)||!write(packet,layout.flags,flags))return false;
    if(interaction)interactionPackets.fetch_add(1);
    return true;
}

int packetId(void* packet) noexcept {
    void* table{};void* getter{};
    if(!read(packet,0,table)||!read(table,packetIdSlot*sizeof(void*),getter)||!inText(getter))return -1;
    std::array<std::uint8_t,8> code{};if(!read(getter,0,code))return -1;
    // Current getId getters are constant leafs. Do not invoke an unverified
    // native ABI or guess a payload layout to identify movement traffic.
    if(code[0]==0xb8&&code[5]==0xc3){unsigned id{};std::memcpy(&id,code.data()+1,4);return id<=65535?static_cast<int>(id):-1;}
    return -1;
}
void BDS_HOOK_CALL packetSubscriber(void*,void* raw,BDS_HookNext next,void* frame) noexcept {
    // Retain the entering flight session BEFORE waiting for restoration. A
    // packet already constructed with proxy coordinates still needs anchoring
    // even if restore clears the active session while this callback waits.
    const auto enteringAnchor=movementAnchor.load();
    auto& args=*static_cast<BDS_PacketSendArgs*>(raw);
    if(args.sender!=observedSender.load()){next(frame,raw);return;}
    const bool auth=packetMatches(args.packet,authPacket),move=packetMatches(args.packet,movePacket);
    if(!auth&&!move){
        // Other traffic is unrelated to movement and must not wait for M.
        const int id=packetId(args.packet);
        if(suppressMovement.load()&&(id==19||id==144)){disable(ReadFailed);return;}
        next(frame,raw);return;
    }
    std::unique_lock phase(packetMotionMutex);
    const auto anchor=enteringAnchor?enteringAnchor:movementAnchor.load();
    if(!anchor) {observeMotionPose(args.packet,auth);phase.unlock();next(frame,raw);return;}
    if(!anchor||!anchorMotionPacket(args.packet,auth,*anchor)) {disable(ReadFailed);blockedPackets.fetch_add(1);return;}
    anchoredPackets.fetch_add(1);phase.unlock();next(frame,raw);
}
void observePacketSender(void* instance) noexcept {
    if(stop.load())return;
    void* sender{};void* table{};void* target{};
    if(!read(instance,senderOffset,sender)||!sender||!read(sender,0,table)||
       !read(table,sendSlot*sizeof(void*),target)||!inText(target))return;
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
    if(packetToken) {
        if(target!=subscribedPacketTarget)return false;
        observedSender.store(sender);packetReady.store(capturedEpoch==epoch.load());return packetReady.load();
    }
    observedSender.store(sender);
    if(!BDS_HookSubscribe(BDS_HOOK_PACKET_SEND_TO_SERVER,target,packetSubscriber,nullptr,&packetToken))return false;
    subscribedPacketTarget=target;packetReady.store(capturedEpoch==epoch.load());
    log("Borion packet source ready: sendToServer RVA="+std::to_string(reinterpret_cast<std::uintptr_t>(target)-reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)))+" [runtime-derived vtable slot; candidate]");
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
        borion.saved=false;suppressMovement.store(false);movementAnchor.store(nullptr);active.store(false);return true;
    }
    auto* state=component(borion.actor,actors.stateVectorComponentHash);
    auto* shape=component(borion.actor,actors.aabbShapeComponentHash);
    auto* fall=component(borion.actor,fallHash);
    if(!state||!shape||!fall)return false;
    // Position restore is a teleport, not a frame-long interpolation across
    // the flight path. Borion also clears motion/fall distance when returning.
    const bool ok=write(state,statePositionOffset,borion.oldPosition)&&
        write(state,statePreviousOffset,borion.oldPosition)&&write(state,stateVelocityOffset,Vec{})&&
        write(shape,actors.aabbMinimumOffset,borion.oldBounds.minimum)&&
        write(shape,actors.aabbMaximumOffset,borion.oldBounds.maximum)&&write(fall,fallOffset,0.f);
    if(ok){borion.saved=false;suppressMovement.store(false);movementAnchor.store(nullptr);active.store(false);}
    return ok;
}
void restoreBorionBeforeBoundary() noexcept {
    std::unique_lock guard(borionMutex,std::try_to_lock);
    if(guard.owns_lock())restoreBorionLocked();
}
void borionTick(const bedrock::LevelTickEvent&) {
    std::unique_lock guard(borionMutex,std::try_to_lock);if(!guard.owns_lock())return;
    observePacketSender(client.load());
    const auto c=config.load();void* player=local.load();
    if(borion.saved && (restoreRequested.load()||stop.load()||!desired.load()||!gameplay.load()||!c->enabled||
       player!=borion.actor||borion.epoch!=epoch.load())) {
        if(!restoreBorionLocked())note(ReadFailed);else restoreRequested.store(false);return;
    }
    if(!borion.saved)restoreRequested.store(false);
    if(stop.load()||!desired.load()||!gameplay.load()||!c->enabled||!player)return;
    if(!packetReady.load()||!movementPacketsSeen.load()){note(Waiting);return;}
    auto* state=component(player,actors.stateVectorComponentHash);
    auto* shape=component(player,actors.aabbShapeComponentHash);auto* fall=component(player,fallHash);
    auto* rotation=component(player,actors.actorRotationComponentHash);
    Vec position{},previous{},velocity{};bedrock::ActorBounds bounds{};float fallDistance{},yaw{};int hurt{};
    if(!state||!shape||!fall||!rotation||!read(state,statePositionOffset,position)||!finite(position)||
       !read(state,statePreviousOffset,previous)||!finite(previous)||!read(state,stateVelocityOffset,velocity)||!finite(velocity)||
       !read(shape,actors.aabbMinimumOffset,bounds.minimum)||!read(shape,actors.aabbMaximumOffset,bounds.maximum)||
       !read(fall,fallOffset,fallDistance)||!std::isfinite(fallDistance)||
       !read(rotation,actors.rotationYawOffset,yaw)||!std::isfinite(yaw)||
       (c->disableOnDamage&&!hurtTime(player,hurt))) {disable(ReadFailed);restoreBorionLocked();return;}
    if(c->disableOnDamage&&hurt>0){disable(Damage);restoreBorionLocked();return;}
    if(!borion.saved) {
        const auto runtime=actorRuntime(player);if(!runtime||!bounds.valid()) {disable(ReadFailed);return;}
        void* registry{};std::uint32_t entity{};
        if(!read(player,actors.actorEntityContextOffset+actors.contextRegistryOffset,registry)||
           !read(player,actors.actorEntityContextOffset+actors.contextEntityIdOffset,entity)){disable(ReadFailed);return;}
        std::unique_lock phase(packetMotionMutex);
        if(!prepareMovementAnchor(runtime,position)){note(Waiting);return;}
        borion={true,player,epoch.load(),runtime,registry,entity,position,previous,velocity,position,bounds,fallDistance};
        suppressMovement.store(true);active.store(true);note(Activated);
    }
    if(actorRuntime(player)!=borion.runtime){disable(ReadFailed);restoreBorionLocked();return;}
    const auto down=[](unsigned key){return keys[key].load()?1.f:0.f;};
    const float forward=down(c->forward)-down(c->back),side=down(c->right)-down(c->left);
    const float radians=yaw*0.0174532925199433f;
    const float length=std::max(1.f,std::sqrt(forward*forward+side*side));
    const float speed=c->speed/20.f*(keys[c->fast].load()?c->fastMultiplier:1.f);
    const Vec old=borion.position;
    // Bedrock yaw: right is the forward vector rotated +90 degrees.
    borion.position.x+=(-std::sin(radians)*forward-std::cos(radians)*side)*speed/length;
    borion.position.z+=(std::cos(radians)*forward-std::sin(radians)*side)*speed/length;
    borion.position.y+=(down(c->up)-down(c->down))*0.5f;
    if(!finite(borion.position)){disable(ReadFailed);restoreBorionLocked();return;}
    const Vec lower{borion.oldBounds.minimum.x+borion.position.x-borion.oldPosition.x,
        borion.oldBounds.minimum.y+borion.position.y-borion.oldPosition.y,
        borion.oldBounds.minimum.z+borion.position.z-borion.oldPosition.z};
    const Vec upper{borion.oldBounds.maximum.x+borion.position.x-borion.oldPosition.x,
        borion.oldBounds.maximum.y+borion.position.y-borion.oldPosition.y,
        borion.oldBounds.maximum.z+borion.position.z-borion.oldPosition.z};
    if(!write(state,statePositionOffset,borion.position)||!write(state,statePreviousOffset,old)||
       !write(state,stateVelocityOffset,Vec{})||!write(fall,fallOffset,0.f)||
       !write(shape,actors.aabbMinimumOffset,lower)||!write(shape,actors.aabbMaximumOffset,upper)) {
        disable(ReadFailed);restoreBorionLocked();return;
    }
    if(c->playerHitboxes){Capture batch;batch.epoch=epoch.load();batch.origin=borion.position;
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
    next(frame,raw);if(!active.load()||stop.load()||!config.load()->playerHitboxes)return;
    static thread_local bool drawing=false;if(drawing)return;
    const auto geometry=mesh.load();if(!geometry||geometry->epoch!=epoch.load()||!geometry->count||Clock::now()-geometry->made>std::chrono::milliseconds(250))return;
    auto& args=*static_cast<BDS_RenderLevelArgs*>(raw);drawing=true;
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
        if(!read(ptr,i/3,value)||value!=expected)throw std::runtime_error("native entry guard mismatch");}
    return const_cast<std::byte*>(ptr);
}
void prepareProfile(const std::string& profile){
    const auto j=nlohmann::json::parse(profile);const auto& f=j.at("native_freecam");
    if(f.at("schema")!=1||f.at("pointer_size")!=8)throw std::runtime_error("unsupported Freecam layout");
    hurtHash=number(f.at("hurt_time_component_hash"));hurtOffset=number(f.at("hurt_time_value_offset"));
    if(hurtOffset>4096||f.at("hurt_time_value_size")!=4)throw std::runtime_error("invalid camera/hurt layout");
    const auto& visual=f.at("visual");
    if(visual.at("schema")!=1||visual.at("spectator_mode_behavior")!="suppress_inside_block_overlay")
        throw std::runtime_error("unsupported view-only spectator profile");
    const auto& b=f.at("borion");
    statePositionOffset=number(b.at("state_position_offset"));statePreviousOffset=number(b.at("state_previous_position_offset"));
    stateVelocityOffset=number(b.at("state_velocity_offset"));fallHash=number(b.at("fall_distance_component_hash"));fallOffset=number(b.at("fall_distance_value_offset"));
    senderOffset=number(b.at("client_packet_sender_offset"));sendSlot=number(b.at("sender_send_to_server_slot"));packetIdSlot=number(b.at("packet_id_slot"));
    if(statePositionOffset!=0||statePreviousOffset!=12||stateVelocityOffset!=24||fallOffset!=0||senderOffset!=0x1d0||sendSlot!=4||packetIdSlot!=1)
        throw std::runtime_error("unsupported Borion native layout");
    checkedFunction(b.at("packet_sender_getter"));
    const auto packetBase=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto& motion=b.at("packet_motion");
    if(motion.at("schema")!=1)throw std::runtime_error("unsupported packet motion schema");
    const auto loadPacket=[&](const nlohmann::json& data,MotionPacketLayout& layout,bool auth){
        layout.vtable=packetBase+number(data.at("vtable_rva"));layout.position=number(data.at("position_offset"));
        layout.vehicle=number(data.at("vehicle_offset"));
        if(layout.position!=0x38||layout.vehicle!=(auth?0xd8:0x58))throw std::runtime_error("unsupported movement packet layout");
        void* getter{};
        if(!read(reinterpret_cast<void*>(layout.vtable),sizeof(void*),getter)||getter!=checkedFunction(data.at("get_id")))
            throw std::runtime_error("movement packet vtable mismatch");
        checkedFunction(data.at("is_valid"));
        if(auth){layout.delta=number(data.at("delta_offset"));layout.analog=number(data.at("analog_move_offset"));
            layout.move=number(data.at("move_offset"));layout.rawMove=number(data.at("raw_move_offset"));layout.flags=number(data.at("input_flags_offset"));
            if(layout.delta!=0x48||layout.analog!=0x5c||layout.move!=0x64||layout.rawMove!=0x80||layout.flags!=0x88||data.at("input_flags_size")!=16)
                throw std::runtime_error("unsupported auth input controls layout");
        }
    };
    loadPacket(motion.at("auth_input"),authPacket,true);loadPacket(motion.at("move_player"),movePacket,false);

    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    moduleTextBegin=base+number(j.at("target").at("text_rva"));moduleTextEnd=moduleTextBegin+number(j.at("target").at("text_size"));
    std::string error;if(!bedrock::parseNativeActorLayout(profile,actors,error)||!bedrock::parseNativeItemLayout(profile,items,error)||
        !bedrock::parseWorldRenderProfile(profile,render,error))throw std::runtime_error(error);
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
        if(!json.contains("full_bright")){json["full_bright"]=true;rewrite=true;}
        const auto next=std::make_shared<const freecam::Config>(freecam::parseConfig(json.dump()));
        if(rewrite){std::ofstream migrated(path,std::ios::trunc);migrated<<json.dump(2)<<'\n';
            if(!migrated)throw std::runtime_error("config update failed");}
        // Key binding changes cannot leave old movement keys held in the game.
        disable();config.store(next);for(auto& key:keys)key.store(false);log("config ready");return true;
    }catch(const std::exception& e){log(std::string("config rejected: ")+e.what());return false;}
}
void worker(const std::filesystem::path& path){
    std::uint64_t generation{};auto lastReload=Clock::now();Clock::time_point nextWaitingLog{},nextPacketLog{};std::error_code ec;auto modified=std::filesystem::last_write_time(path,ec);
    constexpr std::array<std::array<unsigned,2>,12> edges{{{0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7}}};
    while(!stop.load()){
        const auto now=Clock::now();
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
        if((active.load()||interactionPackets.load()||blockedPackets.load())&&now>=nextPacketLog){nextPacketLog=now+std::chrono::seconds(5);
            if(active.load()&&config.load()->spectatorMode)
                log("Freecam inside-block overlay suppressed="+std::to_string(insideBlockSuppressed.exchange(0))+" [view only; game mode unchanged]");
            log("Freecam packet flow: anchored="+std::to_string(anchoredPackets.exchange(0))+" item_inputs="+std::to_string(interactionPackets.exchange(0))+" rejected="+std::to_string(blockedPackets.exchange(0)));
        }
        const auto bits=logBits.exchange(0);
        if(bits&Activated){log("Freecam active [Borion local Player proxy; packet positions anchored; interactions forwarded; Player boxes: every tick + 50ms interpolation]");
            const auto c=config.load();log(std::string("Freecam view: spectator_mode=")+(c->spectatorMode?(insideBlockReady.load()?"inside-block overlay suppressed; game mode unchanged":"unavailable; game mode unchanged"):"off")+" full_bright="+(c->fullBright?(gammaReady.load()?"gamma 25":"unavailable"):"off"));
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
        std::string profile,error;bedrock::ProfileChoice choice;
        if(!bedrock::fetchSupportedProfile(profile,choice,error))throw std::runtime_error(error);
        // An old downloaded exact-build profile can lack a new native section.
        // Embedded fallback is attached and fingerprint-checked by Resolver below.
        if(!nlohmann::json::parse(profile).contains("native_freecam") || !nlohmann::json::parse(profile).at("native_freecam").contains("borion") || !nlohmann::json::parse(profile).at("native_freecam").at("borion").contains("packet_motion") || !nlohmann::json::parse(profile).at("native_freecam").contains("visual") || !nlohmann::json::parse(profile).at("native_freecam").at("visual").contains("inside_block_allowed"))profile=bedrock::generated::kSymbolDatabase;
        bedrock::Resolver resolver;
        if(!resolver.loadJson(profile,error)||!resolver.attachWindowsMainModule(choice.identity,error)||!resolver.exactBuild())throw std::runtime_error("exact-build attach failed: "+error);
        prepareProfile(profile);const auto j=nlohmann::json::parse(profile);const auto& f=j.at("native_freecam");
        // Include selection/load errors even when an old owner's capabilities
        // stop startup before the normal ready message.
        log(BDS_HookRuntimeDiagnostic());
        if(!BDS_HookRuntimeSupports(BDS_HOOK_PACKET_SEND_TO_SERVER))
            throw std::runtime_error("active runtime lacks packet send capability; restart with Runtime 4.2 DLL");
        void* localTarget=checkedFunction(f.at("local_player"));
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));std::array<void*,7> functions{};
        for(unsigned i=0;i<7;++i){const auto& fn=render.functions[i];auto* target=reinterpret_cast<void*>(base+fn.rva);
            if(i==0&&BDS_HookOwnsTarget(BDS_HOOK_RENDER_LEVEL,target)){functions[i]=target;continue;}
            functions[i]=checkedFunction({{"rva",fn.rva},{"entry_check",[&]{std::string s;const char* hex="0123456789abcdef";for(auto b:fn.guard){if(!s.empty())s+=' ';s+=hex[b>>4];s+=hex[b&15];}return s;}()}});
        }
        begin=reinterpret_cast<BeginFn>(functions[1]);vertex=reinterpret_cast<VertexFn>(functions[2]);submit=reinterpret_cast<MeshFn>(functions[4]);
        nameCtor=reinterpret_cast<NameCtor>(functions[5]);materialCtor=reinterpret_cast<MaterialCtor>(functions[6]);materialGroup=reinterpret_cast<void*>(base+render.commonMaterialGroup);
        try {
            void* gammaTarget=checkedFunction(f.at("visual").at("gamma"));
            gammaReady.store(BDS_HookDirectInstall(gammaTarget,reinterpret_cast<void*>(&gammaDetour),reinterpret_cast<void**>(&originalGamma),&gammaToken)!=0);
            log(gammaReady.load()?"Freecam full brightness hook ready [gamma 25 only while active]":"Freecam full brightness unavailable: gamma hook conflict");
        } catch(const std::exception& e){log(std::string("Freecam full brightness unavailable: ")+e.what());}
        try {
            void* target=checkedFunction(f.at("visual").at("inside_block_allowed"));
            insideBlockReady.store(BDS_HookDirectInstall(target,reinterpret_cast<void*>(&insideBlockDetour),reinterpret_cast<void**>(&originalInsideBlock),&insideBlockToken)!=0);
            log(insideBlockReady.load()?"Freecam inside-block view hook ready [camera predicate; no GameType writes; static candidate]":"Freecam inside-block view unavailable: hook conflict");
        } catch(const std::exception& e){log(std::string("Freecam inside-block view unavailable: ")+e.what());}
        api.Event.Enum.Keyboard.then(keyEvent);api.Event.Enum.Mouse.then(mouseEvent);
        api.Event.Enum.Gameplay.then([](const bedrock::GameplayEvent& e){client.store(e.clientInstance);gameplay.store(e.active);if(!e.active){disable();for(auto& key:keys)key.store(false);}});
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
        if(!BDS_HookDirectInstall(localTarget,reinterpret_cast<void*>(&localDetour),reinterpret_cast<void**>(&originalLocal),&localToken)||
           !BDS_HookSubscribe(BDS_HOOK_RENDER_LEVEL,functions[0],renderSubscriber,nullptr,&renderToken))throw std::runtime_error("native hook conflict/install failure; restart Minecraft with matching runtime DLLs");
        log("ready 1.26.52.3; Borion Freecam candidate; press Esc and return to gameplay, then configured toggle key (default M); compatible runtime");log(std::string("active runtime owner: ")+BDS_HookRuntimeVersion());status.store(2);worker(path);
    }catch(const std::exception& e){log(std::string("startup failed: ")+e.what());status.store(4);}
    disable();if(renderToken)BDS_HookUnsubscribe(renderToken);
    // The local-Player direct detour is a lifetime-pinned pass-through after stop.
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
