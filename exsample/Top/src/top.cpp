#ifdef _WIN32
#include <bedrock/client_commands.hpp>
#include <bedrock/game_api.hpp>
#include <bedrock/game_hooks.hpp>
#include <bedrock/generated_symbols.hpp>
#include <bedrock/remote_catalog.hpp>
#include "native.hpp"
#include "native_commands.hpp"
#include <knownfolders.h>
#include <shlobj.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#if !defined(_MSC_VER) || _ITERATOR_DEBUG_LEVEL != 0
#error Top requires MSVC Release STL ABI.
#endif
namespace {
namespace n=bedrock::world_native;namespace tn=top_native;
using Clock=std::chrono::steady_clock;using Vec=bedrock::Vec3;using Bounds=bedrock::ActorBounds;
tn::Bindings bindings;n::Reader reader(bindings);tn::Safety safety(bindings,reader);bedrock::NativeClientCommands nativeCommands;
bedrock::GameApi api;bedrock::ClientCommands commands;std::unique_ptr<bedrock::GameHooks> hooks;
std::atomic<bool> stopped{};std::atomic<unsigned> status{};std::atomic<void*> client{},sender{},sendTarget{};
std::atomic<std::uint64_t> epoch{1};std::uint64_t seenWorld{};
BDS_HookToken packetToken{};std::atomic<void*> installedTarget{};
std::mutex mailMutex,logMutex;
struct Input{std::uint64_t epoch{};std::string line;};std::deque<Input> input;
std::deque<std::string> logs;std::atomic<bool> overflow{};
std::string recentLine;Clock::time_point recentTime{};std::uint64_t recentEpoch{};bool recentNative{},recentPaired{};
std::uint64_t clientTicks{},nextRegistration{};int lastRegistration=-1;
std::string canonical(std::string_view line){
    if(!line.starts_with("/top"))return std::string(line);
    auto arg=line.substr(4);auto a=arg.find_first_not_of(" \t\r\n");if(a==arg.npos)return "/top";
    arg=arg.substr(a,arg.find_last_not_of(" \t\r\n")-a+1);int n{};
    const auto parsed=std::from_chars(arg.data(),arg.data()+arg.size(),n);
    if(parsed.ec==std::errc{}&&parsed.ptr==arg.data()+arg.size())return "/top "+std::to_string(n);
    return std::string(line);
}
bool enqueue(std::string_view line,bool fromNative){
    const auto normalized=canonical(line);const auto world=epoch.load();const auto now=Clock::now();
    std::lock_guard lock(mailMutex);if(stopped.load())return false;
    // Some client routes also forward a packet after local execution. Pair only
    // opposite sources; two deliberate commands from the same source still run.
    if(!recentPaired&&recentEpoch==world&&recentNative!=fromNative&&recentLine==normalized&&now-recentTime<std::chrono::milliseconds(200)){
        recentPaired=true;return true;
    }
    if(input.size()>=16){overflow.store(true);return false;}
    input.push_back({world,normalized});recentLine=normalized;recentTime=now;recentEpoch=world;recentNative=fromNative;recentPaired=false;return true;
}
bedrock::CommandDecision onNativeCommand(std::optional<int> blocks){
    try{
        const auto line=blocks?"/top "+std::to_string(*blocks):std::string("/top");
        return enqueue(line,true)?bedrock::CommandDecision::Accepted:bedrock::CommandDecision::Rejected;
    }catch(...){overflow.store(true);return bedrock::CommandDecision::Rejected;}
}
unsigned maxRise{},levelsPerTick{8},budgetUs{500};bool enabled=true;
void note(std::string s){std::lock_guard lock(logMutex);if(logs.size()<128)logs.push_back(std::move(s));}
void reply(const std::string& text){note(text);const std::optional<std::string> filtered;const std::string tts;
    if(!bindings.message(client.load(),"[Top] "+text,filtered,tts))note("game message unavailable; see Top.log");}
void fail(std::string why){reply("テレポートできません！ "+std::move(why));}
struct Search{void* player{};void* source{};std::uint64_t runtime{},epoch{};Vec position{};Bounds bounds{};int low{},high{},nextY{},lastY{};std::string reason;};
std::optional<Search> search;void* currentPlayer{};void* currentSource{};std::uint64_t currentRuntime{};
void BDS_HOOK_CALL onPacket(void*,void* raw,BDS_HookNext next,void* frame)noexcept{
    auto& args=*static_cast<BDS_PacketSendArgs*>(raw);
    if(stopped.load()||args.sender!=sender.load()){next(frame,raw);return;}
    std::uintptr_t vt{};n::StringCopy copy;
    if(!n::read(args.packet,0,vt)||vt!=bindings.commandVtable||
       !n::copyString(static_cast<const std::byte*>(args.packet)+bindings.commandOffset,&copy)){next(frame,raw);return;}
    try{const std::string_view line(copy.bytes,copy.size);if(!commands.contains(line)){next(frame,raw);return;}
        enqueue(line,false);
        // Only commands registered locally are consumed. The packet is neither
        // retained nor modified; execution is deferred to the owning level tick.
    }catch(...){overflow.store(true);}
}
bool installPacket(){
    void* target=sendTarget.load();if(!target||!sender.load())return false;
    if(packetToken)return target==installedTarget;
    if(!BDS_HookOwnsTarget(BDS_HOOK_PACKET_SEND_TO_SERVER,target)){
        DWORD64 image{};const auto* f=RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(target),&image,nullptr);
        if(!f||image+f->BeginAddress!=reinterpret_cast<DWORD64>(target))return false;
    }
    if(!BDS_HookSubscribe(BDS_HOOK_PACKET_SEND_TO_SERVER,target,onPacket,nullptr,&packetToken))return false;
    installedTarget=target;note("/top registered [local command dispatch; vanilla commands forwarded]");return true;
}
void observeSender(){
    void* s{};void** vt{};void* fn{};
    if(!n::read(client.load(),0x1d0,s)||!n::read(s,0,vt)||!n::read(vt,4*sizeof(void*),fn)||!bindings.executable(fn))return;
    if(installedTarget&&installedTarget.load()!=fn)return;
    sender.store(s);sendTarget.store(fn);
}
bool context(Search& out){
    out.player=currentPlayer;out.source=currentSource;out.runtime=currentRuntime;out.epoch=epoch.load();
    return out.player&&out.source&&bindings.pose(out.player,out.position,out.runtime)&&bindings.bounds(out.player,out.bounds)&&bindings.heights(out.source,out.low,out.high)&&
        std::abs(out.bounds.minimum.x-out.position.x)<4&&std::abs(out.bounds.minimum.y-out.position.y)<4&&std::abs(out.bounds.minimum.z-out.position.z)<4;
}
Bounds atFeet(const Search& s,float feet){auto b=s.bounds;const float dy=feet-b.minimum.y;b.minimum.y+=dy;b.maximum.y+=dy;return b;}
bool finish(const Search& s,float feet){
    Vec now{};std::uint64_t id{};Bounds box{};
    if(s.epoch!=epoch.load()||s.player!=currentPlayer||s.source!=currentSource||!bindings.pose(s.player,now,id)||id!=s.runtime||!bindings.bounds(s.player,box)){
        fail("ワールドまたはプレイヤーが変わりました");return false;}
    if(std::abs(now.x-s.position.x)>.02f||std::abs(now.y-s.position.y)>.02f||std::abs(now.z-s.position.z)>.02f||
       std::abs(box.minimum.y-s.bounds.minimum.y)>.02f||std::abs(box.maximum.x-s.bounds.maximum.x)>.02f||std::abs(box.maximum.y-s.bounds.maximum.y)>.02f||std::abs(box.minimum.x-s.bounds.minimum.x)>.02f||std::abs(box.minimum.z-s.bounds.minimum.z)>.02f||std::abs(box.maximum.z-s.bounds.maximum.z)>.02f){fail("探索中に位置または姿勢が変わりました。もう一度実行してください");return false;}
    const auto destination=atFeet(s,feet);
    if(auto reason=safety.check(s.source,destination,s.low,s.high);!reason.empty()){fail(reason);return false;}
    if(!bindings.move(s.player,feet-s.bounds.minimum.y)){fail("位置情報の更新に失敗しました");return false;}
    reply("移動しました。足元 Y="+std::to_string(feet));return true;
}
void execute(std::string_view args){
    search.reset();const auto request=top_mod::parse(args);
    if(!request){fail("使い方: /top または /top <正の整数ブロック数>");return;}
    if(!enabled){fail("設定で無効になっています");return;}
    Search s;if(!context(s)){fail("現在のプレイヤー・当たり判定・高さ範囲を取得できません");return;}
    const auto feet=s.bounds.minimum.y;
    if(request->exact){
        if(maxRise&&request->distance>maxRise){fail("設定された移動距離の上限を超えています");return;}
        const double y=double(feet)+request->distance;
        if(y+(s.bounds.maximum.y-feet)>s.high){fail("ディメンションの高さ上限を超えています");return;}
        finish(s,static_cast<float>(y));return;
    }
    s.nextY=static_cast<int>(std::floor(s.bounds.maximum.y-.0001f));
    s.lastY=s.high-1;
    if(maxRise){const double cap=std::floor(double(feet)+maxRise-1);if(cap<s.lastY)s.lastY=static_cast<int>(cap);}
    if(s.nextY>s.lastY){fail("頭上に探索可能な範囲がありません");return;}
    search=std::move(s);note("upward search started; no configured distance limit="+std::to_string(maxRise==0));
}
void tickGame(const bedrock::LevelTickEvent& e){
    const auto world=epoch.load();
    if(seenWorld!=world){search.reset();reader.clear();{std::lock_guard lock(mailMutex);input.clear();}seenWorld=world;nativeCommands.reset();nextRegistration=0;lastRegistration=-1;}
    if(stopped.load()){search.reset();return;}
    observeSender();
    if(++clientTicks>=nextRegistration){
        nextRegistration=clientTicks+20;
        const auto result=nativeCommands.update(client.load());const auto value=static_cast<int>(result);
        if(result==bedrock::NativeClientCommands::Result::registered)note("/top native CommandRegistry registered [SyncLocal + NotCheat; blocks: optional int; native autocomplete + local execution]");
        else if(value!=lastRegistration&&result==bedrock::NativeClientCommands::Result::collision)note("/top native command name already exists; existing metadata preserved; packet fallback remains available");
        else if(value!=lastRegistration&&result==bedrock::NativeClientCommands::Result::failed)note("/top native registration failed: "+nativeCommands.error());
        lastRegistration=value;
    }
    std::optional<Input> command;{std::lock_guard lock(mailMutex);if(!input.empty()){command=std::move(input.front());input.pop_front();}}
    if(overflow.exchange(false))fail("コマンドが多すぎます。少し待ってから実行してください");
    if(!search&&!command)return; // No Actor, ECS, region or block reads while idle.
    currentPlayer=bindings.local(client.load());currentSource=bindings.region(client.load());currentRuntime=0;
    Vec pos{};void* level{};
    if(!currentPlayer||!currentSource||!bindings.pose(currentPlayer,pos,currentRuntime)||!n::read(currentPlayer,0x1d8,level)||level!=e.level){search.reset();if(command)fail("現在のワールドを取得できません");return;}
    if(command&&command->epoch==world)commands.dispatch(command->line);
    if(!search)return;
    auto& s=*search;
    if(s.epoch!=world||s.player!=currentPlayer||s.runtime!=currentRuntime||s.source!=currentSource){search.reset();fail("ワールドまたはプレイヤーが変わりました");return;}
    if(std::abs(pos.x-s.position.x)>.02f||std::abs(pos.y-s.position.y)>.02f||std::abs(pos.z-s.position.z)>.02f){search.reset();fail("探索中に移動しました。もう一度実行してください");return;}
    const auto deadline=Clock::now()+std::chrono::microseconds(budgetUs);
    for(unsigned i=0;i<levelsPerTick&&s.nextY<=s.lastY;++i){
        const int y=s.nextY++;auto* state=bindings.block(s.source,{static_cast<int>(std::floor(pos.x)),y,static_cast<int>(std::floor(pos.z))});
        auto* info=reader.describe(state);
        if(!info){search.reset();fail("頭上のチャンクまたはブロックを読み取れません");return;}
        if(!tn::air(info->name)){
            const float feet=float(y+1);auto reason=safety.check(s.source,atFeet(s,feet),s.low,s.high);
            if(reason.empty()){const auto ready=s;search.reset();finish(ready,feet);return;}
            s.reason=std::move(reason);
        }
        if(Clock::now()>=deadline)break;
    }
    if(s.nextY>s.lastY){const auto reason=s.reason.empty()?std::string("頭上に安全な着地点がありません"):"頭上に安全な着地点がありません（"+s.reason+"）";search.reset();fail(reason);}
}
void onTick(const bedrock::LevelTickEvent& event){try{tickGame(event);}catch(const std::exception& ex){search.reset();note(std::string("tick failed: ")+ex.what());fail("内部処理に失敗しました");}}
std::filesystem::path directory(){PWSTR path{};if(FAILED(SHGetKnownFolderPath(FOLDERID_Documents,KF_FLAG_CREATE,nullptr,&path)))throw std::runtime_error("Documents unavailable");auto dir=std::filesystem::path(path)/L"PEXData"/L"Top";CoTaskMemFree(path);return dir;}
void loadConfig(const std::filesystem::path& dir){
    const auto p=dir/L"config.json";nlohmann::json j;
    if(std::filesystem::exists(p)){std::ifstream f(p);f>>j;}else{j={{"enabled",true},{"max_rise",0},{"scan_levels_per_tick",8},{"cooperative_budget_us",500}};std::ofstream f(p);f<<j.dump(2)<<'\n';}
    enabled=j.value("enabled",true);
    const auto rise=j.value("max_rise",0LL);if(rise<0||rise>65535)throw std::runtime_error("max_rise must be 0..65535 (0 means unlimited)");maxRise=static_cast<unsigned>(rise);
    levelsPerTick=static_cast<unsigned>(std::clamp(j.value("scan_levels_per_tick",8),1,32));budgetUs=static_cast<unsigned>(std::clamp(j.value("cooperative_budget_us",500),100,2000));
}
DWORD WINAPI start(void*){
    HMODULE pinned{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&start),&pinned);
    status.store(1);
    try{const auto dir=directory();std::filesystem::create_directories(dir);loadConfig(dir);std::string error;
        bedrock::ProfileManager profiles;
        commands.add("top",execute);
        auto definition=bedrock::CommandBuilder::named(bedrock::CommandName{"top"})
            .description(bedrock::CommandDescription{"安全な上方移動 / safe upward teleport"})
            .optionalInteger(bedrock::ArgumentName{"blocks"}).handles(onNativeCommand);
        if(!nativeCommands.add(std::move(definition)))throw std::runtime_error("native command definition rejected");
        const bedrock::ProfileRequirements required{bedrock::Feature::World,bedrock::Feature::Motion,bedrock::Feature::Gui,
            bedrock::Feature::WorldHeight,bedrock::Feature::CommandPackets,bedrock::Feature::ClientCommands,bedrock::Feature::WorldEvents};
        const bedrock::ProfileOptions options{bedrock::UpdatePolicy::RefreshIfNeeded,bedrock::CandidatePolicy::AllowCandidates};
        if(!profiles.prepare(required,options,[](const bedrock::RuntimeProfile& selected,std::string& failure){
            try{bindings.loadTop(selected.json());return nativeCommands.load(selected,failure);}
            catch(const std::exception& e){failure=e.what();return false;}
        },error))throw std::runtime_error(error);
        const auto selected=profiles.snapshot();const auto& profile=selected->json();const auto& resolver=selected->resolver();
        api.Event.Enum.Gameplay.then([](const bedrock::GameplayEvent& e){if(e.clientInstance)client.store(e.clientInstance);});
        api.Event.Enum.WorldChange.then([](const bedrock::WorldChangeEvent& e){if(e.clientInstance)client.store(e.clientInstance);nativeCommands.reset();sender.store(nullptr);sendTarget.store(nullptr);++epoch;});
        api.Event.Enum.LevelTick.then(onTick);hooks=std::make_unique<bedrock::GameHooks>(api);
        if(!hooks->installGameplay(resolver,error)||!hooks->installLevelTick(resolver,error)||!hooks->installWorldEvents(resolver,profile,error))throw std::runtime_error(error);
        note("Top 1.26.52.3 candidate revision2; native registry + autocomplete; /top and /top N; world reads/writes on client tick only");note("Press Esc and return to gameplay if the client has not been observed yet.");note(BDS_HookRuntimeDiagnostic());status.store(2);
        std::ofstream out(dir/L"Top.log",std::ios::trunc);
        while(!stopped.load()){
            installPacket();std::deque<std::string> batch;{std::lock_guard lock(logMutex);batch.swap(logs);}for(const auto& line:batch)out<<line<<'\n';if(!batch.empty())out.flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }catch(const std::exception& e){status.store(4);try{auto dir=directory();std::filesystem::create_directories(dir);std::ofstream out(dir/L"Top.log",std::ios::app);out<<"startup failed: "<<e.what()<<'\n';}catch(...){} }
    stopped.store(true);if(status.load()!=4)status.store(3);return 0;
}
}
extern "C" __declspec(dllexport) unsigned Top_GetStatus(){return status.load();}
extern "C" __declspec(dllexport) void Top_Stop(){stopped.store(true);}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){DisableThreadLibraryCalls(instance);auto t=CreateThread(nullptr,0,start,nullptr,0,nullptr);if(t)CloseHandle(t);else status.store(4);}else if(reason==DLL_PROCESS_DETACH)stopped.store(true);return TRUE;}
#endif
