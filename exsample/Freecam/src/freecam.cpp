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
struct Quaternion {float x,y,z,w;};
struct Box {std::uint64_t id{};bedrock::ActorBounds bounds{};};
struct Capture {std::uint64_t epoch{};std::array<Box,16> boxes{};unsigned count{};Vec origin{};};
struct Mesh {std::uint64_t epoch{};Clock::time_point made{};unsigned count{};std::array<Vec,128*24> vertices{};};
struct State {Vec position{};float yaw{},pitch{};void* actor{};std::uint64_t epoch{};Clock::time_point last{};int hurt{};};
std::atomic<bool> stop{},desired{},active{},gameplay{};
std::atomic<unsigned> status{};
std::atomic<void*> client{},local{};
std::atomic<std::uint64_t> epoch{1};
std::array<std::atomic<bool>,256> keys{};
std::atomic<float> mouseX{},mouseY{};
std::atomic<std::shared_ptr<const freecam::Config>> config{std::make_shared<const freecam::Config>()};
bedrock::GameApi api;std::unique_ptr<bedrock::GameHooks> hooks;
bedrock::NativeActorLayout actors;bedrock::NativeItemLayout items;bedrock::WorldRenderProfile render;
std::size_t cameraPosition{},cameraOrientation{},hurtOffset{};std::uint32_t hurtHash{};
BDS_HookToken cameraToken{},localToken{},renderToken{};
using CameraFn=void(__fastcall*)(void*,void*,void*);CameraFn originalCamera{};
using LocalFn=void*(__fastcall*)(void*);LocalFn originalLocal{};
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
void disable(unsigned reason=Disabled){desired.store(false);active.store(false);mouseX.store(0);mouseY.store(0);mesh.store(nullptr);note(reason);}
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
void* __fastcall localDetour(void* instance){void* player=originalLocal(instance);client.store(instance);local.store(player);return player;}
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
    if(releasing)return;const auto c=config.load();const unsigned k=e.inputKey();if(k<keys.size())keys[k].store(e.pressed);
    if(k==c->toggleKey){
        if(e.pressed&&!e.repeat&&gameplay.load()&&c->enabled){
            if(desired.load())disable();else{releaseMovement(*c);desired.store(true);}
        }
        if(gameplay.load()) e.cancel();return;
    }
    if(desired.load()&&isMovement(k,*c))e.cancel();
}
void mouseEvent(const bedrock::MouseEvent& e){
    if(releasing||!desired.load())return;const auto c=config.load();
    if(e.action==0){mouseX.fetch_add(static_cast<float>(e.deltaX));mouseY.fetch_add(static_cast<float>(e.deltaY));e.cancel();}
    else if(!c->allowInteraction && (e.action==1||e.action==2||e.action==3||e.action==4))e.cancel();
}
bool capturePlayers(void* actor,Capture* batch,unsigned limit) noexcept {
    void* registry{};if(!read(actor,actors.actorEntityContextOffset+actors.contextRegistryOffset,registry)||!registry)return false;
    static thread_local std::size_t cursor=0;static thread_local void* previousRegistry{};static thread_local std::uint64_t previousEpoch{};
    if(previousRegistry!=registry||previousEpoch!=batch->epoch){cursor=0;previousRegistry=registry;previousEpoch=batch->epoch;}
    __try {
        auto* r=static_cast<Registry*>(registry);auto* types=r->storage(actors.actorTypeComponentHash);
        auto* owners=r->storage(actors.actorOwnerComponentHash);auto* bounds=r->storage(actors.aabbShapeComponentHash);
        auto* ids=r->storage(actors.runtimeIdComponentHash);if(!types||!owners||!bounds||!ids)return false;
        const auto size=types->size();if(size>16384)return false;const auto start=Clock::now();
        for(unsigned n=0;n<limit&&n<size;++n){
            if(n&&Clock::now()-start>std::chrono::microseconds(150))break;
            if(cursor>=size)cursor=0;auto entity=types->data()[cursor++];std::uint32_t type{};void* owner{};Box box;
            if(!r->valid(entity)||!types->contains(entity)||!read(types->value(entity),render.actorTypeValueOffset,type)||type!=render.playerTypeId||
               !owners->contains(entity)||!read(owners->value(entity),actors.actorOwnerPointerOffset,owner)||owner==actor||
               !bounds->contains(entity)||!ids->contains(entity)||!read(ids->value(entity),actors.runtimeIdValueOffset,box.id)||!box.id||
               !read(bounds->value(entity),actors.aabbMinimumOffset,box.bounds.minimum)||
               !read(bounds->value(entity),actors.aabbMaximumOffset,box.bounds.maximum)||!box.bounds.valid()||
               !finite(box.bounds.minimum)||!finite(box.bounds.maximum))continue;
            const auto a=box.bounds.minimum,b=box.bounds.maximum;
            if(b.x-a.x>4||b.y-a.y>5||b.z-a.z>4)continue;
            batch->boxes[batch->count++]=box;
        }
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void cameraUpdate(void* camera,void* player){
    static thread_local State s;
    const auto c=config.load();const auto currentEpoch=epoch.load();
    if(stop.load()||!desired.load()||!gameplay.load()||!c->enabled||player!=local.load()) {active.store(false);return;}
    Vec position{};Quaternion q{};int hurt{};
    if(!read(camera,cameraPosition,position)||!finite(position)||!read(camera,cameraOrientation,q)||
       (c->disableOnDamage&&!hurtTime(player,hurt))){disable(ReadFailed);return;}
    if(!active.load()||s.actor!=player||s.epoch!=currentEpoch){
        if(c->disableOnDamage&&hurt>0){disable(Damage);return;}
        const float norm=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
        if(!std::isfinite(norm)||norm<0.9f||norm>1.1f){disable(ReadFailed);return;}
        const float fx=-2*(q.x*q.z+q.w*q.y),fy=2*(q.w*q.x-q.y*q.z),fz=-(1-2*(q.x*q.x+q.y*q.y));
        s={position,std::atan2(-fx,fz)*57.2957795f,-std::asin(std::clamp(fy,-1.f,1.f))*57.2957795f,player,currentEpoch,Clock::now(),hurt};
        active.store(true);mouseX.store(0);mouseY.store(0);note(Activated);
    }
    if(c->disableOnDamage && hurt>s.hurt){disable(Damage);return;}s.hurt=hurt;
    const auto now=Clock::now();const float dt=std::clamp(std::chrono::duration<float>(now-s.last).count(),0.f,0.1f);s.last=now;
    s.yaw=std::remainder(s.yaw+mouseX.exchange(0)*c->sensitivity,360.f);
    s.pitch=std::clamp(s.pitch+mouseY.exchange(0)*c->sensitivity,-89.9f,89.9f);
    const auto down=[](unsigned k){return keys[k].load()?1.f:0.f;};
    const float f=down(c->forward)-down(c->back),r=down(c->right)-down(c->left),u=down(c->up)-down(c->down);
    const float yaw=s.yaw*0.0174532925f,pitch=s.pitch*0.0174532925f;
    Vec move{-std::sin(yaw)*std::cos(pitch)*f+std::cos(yaw)*r,-std::sin(pitch)*f+u,std::cos(yaw)*std::cos(pitch)*f+std::sin(yaw)*r};
    const float length=std::sqrt(move.x*move.x+move.y*move.y+move.z*move.z);
    if(length>0){const float step=dt*c->speed*(keys[c->fast].load()?c->fastMultiplier:1.f)/std::max(1.f,length);
        s.position.x+=move.x*step;s.position.y+=move.y*step;s.position.z+=move.z*step;}
    // Minecraft camera quaternion convention: forward is local -Z.
    const float hy=(yaw+3.14159265359f)*0.5f,hp=-pitch*0.5f;
    q={std::cos(hy)*std::sin(hp),std::sin(hy)*std::cos(hp),-std::sin(hy)*std::sin(hp),std::cos(hy)*std::cos(hp)};
    if(currentEpoch!=epoch.load()||!desired.load())return;
    if(!write(camera,cameraPosition,s.position)||!write(camera,cameraOrientation,q)){disable(ReadFailed);return;}
    if(c->playerHitboxes){Capture batch;batch.epoch=currentEpoch;batch.origin=s.position;
        if(capturePlayers(player,&batch,c->captureRows)){
            std::unique_lock lock(mailboxMutex,std::try_to_lock);if(lock.owns_lock()){mailbox=batch;mailboxReady=true;}
        }
    }
}
void __fastcall cameraDetour(void* camera,void* mode,void* player){
    // Preserve normal camera->Player rotation when inactive. While detached,
    // skip that propagation; server Player coordinates are never overwritten.
    if(desired.load()&&player==local.load()){
        try{cameraUpdate(camera,player);}catch(...){disable(ReadFailed);}
        if(active.load())return;
    }
    originalCamera(camera,mode,player);
}
