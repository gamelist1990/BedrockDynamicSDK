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
    const float hy=(3.14159265359f-yaw)*0.5f,hp=-pitch*0.5f;
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
            for(unsigned i=0;i<geometry->count;++i){const auto v=geometry->vertices[i];vertex(tess,v.x-origin.x,v.y-origin.y,v.z-origin.z);}
            alignas(16) std::array<std::byte,0x58> scratch{};submit(screen,tess,material.data(),scratch.data());success=true;
        }
    }__except(EXCEPTION_EXECUTE_HANDLER){success=false;}
    write(shader,0,oldColor);write(shader,render.dirty,dirty);write(tess,render.translation,oldTranslation);
    return success;
}
void BDS_HOOK_CALL renderSubscriber(void*,void* raw,BDS_HookNext next,void* frame){
    next(frame,raw);if(!active.load()||stop.load()||!config.load()->playerHitboxes)return;
    static thread_local bool drawing=false;if(drawing)return;
    const auto geometry=mesh.load();if(!geometry||geometry->epoch!=epoch.load()||!geometry->count||Clock::now()-geometry->made>std::chrono::seconds(2))return;
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
    cameraPosition=number(f.at("camera_position_offset"));cameraOrientation=number(f.at("camera_orientation_offset"));
    hurtHash=number(f.at("hurt_time_component_hash"));hurtOffset=number(f.at("hurt_time_value_offset"));
    if(cameraPosition!=64||cameraOrientation!=48||hurtOffset>4096||f.at("hurt_time_value_size")!=4)throw std::runtime_error("invalid camera/hurt layout");
    std::string error;if(!bedrock::parseNativeActorLayout(profile,actors,error)||!bedrock::parseNativeItemLayout(profile,items,error)||
        !bedrock::parseWorldRenderProfile(profile,render,error))throw std::runtime_error(error);
}
std::filesystem::path directory(){
    PWSTR path{};if(FAILED(SHGetKnownFolderPath(FOLDERID_Documents,KF_FLAG_CREATE,nullptr,&path)))throw std::runtime_error("Documents folder unavailable");
    std::filesystem::path result(path);CoTaskMemFree(path);return result/L"PEXData"/L"Freecam";
}
bool loadConfig(const std::filesystem::path& path){
    try{std::ifstream in(path);if(!in)throw std::runtime_error("cannot open config");const std::string text((std::istreambuf_iterator<char>(in)),{});
        const auto next=std::make_shared<const freecam::Config>(freecam::parseConfig(text));
        // Key binding changes cannot leave old movement keys held in the game.
        disable();config.store(next);for(auto& key:keys)key.store(false);log("config ready");return true;
    }catch(const std::exception& e){log(std::string("config rejected: ")+e.what());return false;}
}
void worker(const std::filesystem::path& path){
    struct Entry {Box box;Clock::time_point seen;};std::unordered_map<std::uint64_t,Entry> tracked;
    std::uint64_t generation{};auto lastReload=Clock::now();std::error_code ec;auto modified=std::filesystem::last_write_time(path,ec);
    constexpr std::array<std::array<unsigned,2>,12> edges{{{0,1},{1,3},{3,2},{2,0},{4,5},{5,7},{7,6},{6,4},{0,4},{1,5},{2,6},{3,7}}};
    while(!stop.load()){
        const auto now=Clock::now();const auto e=epoch.load();if(generation!=e){generation=e;tracked.clear();mesh.store(nullptr);}
        Capture batch;bool have=false;{std::lock_guard lock(mailboxMutex);if(mailboxReady){batch=mailbox;mailboxReady=false;have=true;}}
        if(have&&batch.epoch==generation&&active.load()){
            for(unsigned i=0;i<batch.count;++i)tracked[batch.boxes[i].id]={batch.boxes[i],now};
            const auto c=config.load();auto result=std::make_shared<Mesh>();result->epoch=generation;result->made=now;unsigned boxes=0;
            for(auto it=tracked.begin();it!=tracked.end();){
                if(now-it->second.seen>std::chrono::seconds(2)){it=tracked.erase(it);continue;}
                const auto& b=it->second.box.bounds;const float x=(b.minimum.x+b.maximum.x)*0.5f-batch.origin.x,
                    y=(b.minimum.y+b.maximum.y)*0.5f-batch.origin.y,z=(b.minimum.z+b.maximum.z)*0.5f-batch.origin.z;
                if(boxes<c->maxPlayers && x*x+y*y+z*z<=c->radius*c->radius){
                    for(const auto& edge:edges)for(unsigned corner:edge){
                        result->vertices[result->count++]={(corner&1)?b.maximum.x:b.minimum.x,(corner&2)?b.maximum.y:b.minimum.y,(corner&4)?b.maximum.z:b.minimum.z};
                    }++boxes;
                }++it;
            }
            if(tracked.size()>1024)tracked.clear();
            if(active.load()&&generation==epoch.load())mesh.store(result);
        }
        if(!active.load()){tracked.clear();mesh.store(nullptr);}
        const auto bits=logBits.exchange(0);
        if(bits&Activated)log("Freecam active [camera only; Player position unchanged]");
        if(bits&Disabled)log("Freecam disabled");if(bits&Damage)log("Freecam disabled: local Player hurt");
        if(bits&ReadFailed)log("Freecam disabled: native camera/Player read failed");
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
        if(!nlohmann::json::parse(profile).contains("native_freecam"))profile=bedrock::generated::kSymbolDatabase;
        bedrock::Resolver resolver;
        if(!resolver.loadJson(profile,error)||!resolver.attachWindowsMainModule(choice.identity,error)||!resolver.exactBuild())throw std::runtime_error("exact-build attach failed: "+error);
        prepareProfile(profile);const auto j=nlohmann::json::parse(profile);const auto& f=j.at("native_freecam");
        void* cameraTarget=checkedFunction(f.at("update_player"));void* localTarget=checkedFunction(f.at("local_player"));
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));std::array<void*,7> functions{};
        for(unsigned i=0;i<7;++i){const auto& fn=render.functions[i];auto* target=reinterpret_cast<void*>(base+fn.rva);
            if(i==0&&BDS_HookOwnsTarget(BDS_HOOK_RENDER_LEVEL,target)){functions[i]=target;continue;}
            functions[i]=checkedFunction({{"rva",fn.rva},{"entry_check",[&]{std::string s;const char* hex="0123456789abcdef";for(auto b:fn.guard){if(!s.empty())s+=' ';s+=hex[b>>4];s+=hex[b&15];}return s;}()}});
        }
        begin=reinterpret_cast<BeginFn>(functions[1]);vertex=reinterpret_cast<VertexFn>(functions[2]);submit=reinterpret_cast<MeshFn>(functions[4]);
        nameCtor=reinterpret_cast<NameCtor>(functions[5]);materialCtor=reinterpret_cast<MaterialCtor>(functions[6]);materialGroup=reinterpret_cast<void*>(base+render.commonMaterialGroup);
        api.Event.Enum.Keyboard.then(keyEvent);api.Event.Enum.Mouse.then(mouseEvent);
        api.Event.Enum.Gameplay.then([](const bedrock::GameplayEvent& e){client.store(e.clientInstance);gameplay.store(e.active);if(!e.active){disable();for(auto& key:keys)key.store(false);}});
        api.Event.Enum.WorldChange.then([](const bedrock::WorldChangeEvent&){reset();});
        hooks=std::make_unique<bedrock::GameHooks>(api);
        if(!hooks->installInput(resolver,error)||!hooks->installGameplay(resolver,error)||!hooks->installWorldEvents(resolver,profile,error))throw std::runtime_error(error);
        if(!BDS_HookDirectInstall(localTarget,reinterpret_cast<void*>(&localDetour),reinterpret_cast<void**>(&originalLocal),&localToken)||
           !BDS_HookDirectInstall(cameraTarget,reinterpret_cast<void*>(&cameraDetour),reinterpret_cast<void**>(&originalCamera),&cameraToken)||
           !BDS_HookSubscribe(BDS_HOOK_RENDER_LEVEL,functions[0],renderSubscriber,nullptr,&renderToken))throw std::runtime_error("native hook conflict/install failure; restart Minecraft with matching runtime DLLs");
        log("ready 1.26.52.3; native Freecam candidate; press Esc and return to gameplay, then V; Runtime 4.0.0");status.store(2);worker(path);
    }catch(const std::exception& e){log(std::string("startup failed: ")+e.what());status.store(4);}
    disable();if(renderToken)BDS_HookUnsubscribe(renderToken);
    // The two direct detours are lifetime-pinned pass-throughs after stop.
    // Do not tear down trampolines while a native thread may still enter them.
    stop.store(true);if(hooks)hooks->stop();if(status.load()!=4)status.store(3);return 0;
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
