#ifdef _WIN32
#include <bedrock/native_world.hpp>
#include <bedrock/native_outline_renderer.hpp>
#include <bedrock/world_text_win32.hpp>
#include <bedrock/game_hooks.hpp>
#include "block_outline/config.hpp"
#include <knownfolders.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#if !defined(_MSC_VER) || _ITERATOR_DEBUG_LEVEL != 0
#error BlockOutline requires MSVC Release x64 native ABI.
#endif
namespace {
namespace bo = block_outline;
using Clock = std::chrono::steady_clock;
using Vec = bedrock::Vec3;
using Pos = bedrock::BlockPos;
using namespace std::chrono_literals;
constexpr std::size_t maximumMatches=1024, maximumTextVertices=65536;
struct Match { Pos position; unsigned target{}; };
struct Capture {
    std::uint64_t scene{};
    Vec player{};
    std::shared_ptr<const bo::Config> config;
    std::array<Match,maximumMatches> matches{};
    std::size_t count{};
    Clock::time_point captured{};
};
struct Prepared {
    std::uint64_t scene{}, revision{};
    Clock::time_point captured{};
    bedrock::WorldOutlineGeometry geometry;
};
float distance(Vec a,Vec b) { const auto x=a.x-b.x,y=a.y-b.y,z=a.z-b.z; return x*x+y*y+z*z; }
Vec center(Pos p) { return {float(p.x)+.5f,float(p.y)+.5f,float(p.z)+.5f}; }
bool inside(Pos p,Vec player,float range) { return distance(center(p),player)<=range*range; }
std::filesystem::path directory() {
    PWSTR path{};
    if(FAILED(SHGetKnownFolderPath(FOLDERID_Documents,KF_FLAG_CREATE,nullptr,&path)))throw std::runtime_error("Documents unavailable");
    const auto documents=std::filesystem::path(path); CoTaskMemFree(path);
    return documents/L"PEXData"/L"BlockOutline";
}
class BlockOutline {
public:
    std::atomic<unsigned> status{};
    void signalStop() noexcept { stopped_.store(true); }
    void stop() noexcept { signalStop(); prepared_.store(nullptr); }
    void run() {
        status=1;
        try {
            dir_=directory(); std::filesystem::create_directories(dir_);
            out_.open(dir_/L"BlockOutline.log",std::ios::trunc);
            if(!std::filesystem::exists(dir_/L"config.json")) {
                std::ofstream file(dir_/L"config.json"); file<<bo::defaults().dump(2)<<'\n';
            }
            reload(true); initialize(); status=2;
            bedrock::world_text::NameMesher mesher;
            while(!stopped_.load()) {
                if(Clock::now()>=nextConfig_) { reload(false); nextConfig_=Clock::now()+1s; }
                Capture capture; bool available=false;
                { std::lock_guard lock(mailMutex_); if(mailReady_) { capture=mail_; mailReady_=false; available=true; } }
                if(available && capture.scene==scene_.load() && capture.config->revision==config_.load()->revision) {
                    auto geometry=prepare(capture,mesher);
                    if(!stopped_.load() && capture.scene==scene_.load() && capture.config->revision==config_.load()->revision)
                        prepared_.store(std::move(geometry));
                }
                if(renderFault_.exchange(false))note("world draw fault; drawing disabled until config reload or world change");
                flush(); std::this_thread::sleep_for(100ms);
            }
            flush(); status=3;
        } catch(const std::exception& error) {
            note(std::string("startup/worker failed: ")+error.what()); flush(); status=4;
        }
        stop();
    }
private:
    void note(std::string text) { std::lock_guard lock(logMutex_); if(logs_.size()<128)logs_.push_back(std::move(text)); }
    void flush() {
        std::vector<std::string> lines;
        { std::lock_guard lock(logMutex_); lines.swap(logs_); }
        for(const auto& line:lines)out_<<line<<'\n';
        if(!lines.empty())out_.flush();
    }
    void reload(bool initial) {
        try {
            const auto modified=std::filesystem::last_write_time(dir_/L"config.json");
            if(!initial && modified==modified_)return;
            modified_=modified;
            std::ifstream file(dir_/L"config.json"); nlohmann::json value; file>>value;
            auto config=std::make_shared<bo::Config>(bo::parse(value)); config->revision=++configRevision_;
            config_.store(std::move(config)); prepared_.store(nullptr); drawDisabled_.store(false);
            fonts_.clear(); note("config ready: targets="+std::to_string(config_.load()->targets.size())+" range="+std::to_string(config_.load()->range));
        } catch(const std::exception& error) {
            if(initial)throw;
            note(std::string("config rejected; previous settings kept: ")+error.what());
        }
    }
    void boundary() {
        epoch_.fetch_add(1); scene_.fetch_add(1); prepared_.store(nullptr); drawDisabled_.store(false);
    }
    void initialize() {
        bedrock::ProfileManager profiles;
        const bedrock::ProfileRequirements required{bedrock::Feature::Gameplay,bedrock::Feature::LevelTick,
            bedrock::Feature::World,bedrock::Feature::WorldRender,bedrock::Feature::WorldEvents};
        const bedrock::ProfileOptions options{bedrock::UpdatePolicy::RefreshIfNeeded,bedrock::CandidatePolicy::AllowCandidates};
        std::string error;
        if(!profiles.prepare(required,options,[this](const bedrock::RuntimeProfile& profile,std::string& failure) {
            if(!world_.load(profile,failure)||!renderer_.load(profile,failure))return false;
            if(!world_.supportsSubChunkScan()) { failure="SubChunk scan layout missing; refresh exact-build profile"; return false; }
            return true;
        },error))throw std::runtime_error(error);
        api_.Event.Enum.Gameplay.then([this](const bedrock::GameplayEvent& event) { if(event.clientInstance)client_.store(event.clientInstance); });
        api_.Event.Enum.WorldChange.then([this](const bedrock::WorldChangeEvent& event) {
            if(event.clientInstance)client_.store(event.clientInstance); boundary();
        });
        api_.Event.Enum.LevelTick.then([this](const bedrock::LevelTickEvent& event) {
            try { tick(event); } catch(const std::exception& error) {
                prepared_.store(nullptr); note(std::string("scan deferred: ")+error.what());
            }
        });
        hooks_=std::make_unique<bedrock::GameHooks>(api_);
        const auto profile=profiles.snapshot();
        if(!hooks_->installGameplay(profile->resolver(),error)||!hooks_->installLevelTick(profile->resolver(),error)||
            !hooks_->installWorldEvents(profile->resolver(),profile->json(),error))throw std::runtime_error(error);
        if(!BDS_HookSubscribe(BDS_HOOK_RENDER_LEVEL,renderer_.renderTarget(),renderCallback,this,&renderToken_))
            throw std::runtime_error("world render subscription failed");
        note("BlockOutline ready; SubChunk storage scan + empty-section skip + copied-geometry worker + Unicode labels");
        note("Press Esc and return to gameplay if the client has not been observed yet."); note(BDS_HookRuntimeDiagnostic());
    }
    void restart(Vec position,const bo::Config& config) {
        anchor_={int(std::floor(position.x)),int(std::floor(position.y)),int(std::floor(position.z))};
        cursor_.reset(anchor_,config.range); pending_.reset(); matches_.clear(); check_=matches_.end();
    }
    void update(Pos position,const bedrock::BlockResult& result,const bo::Config& config,Vec player) {
        const auto old=matches_.find(position);
        const auto target=result && result.block.kind==bedrock::CellKind::block ? config.lookup.find(result.block.identifier) : config.lookup.end();
        if(!inside(position,player,config.range)||target==config.lookup.end()) {
            if(old!=matches_.end()) { if(check_==old)++check_; matches_.erase(old); }
        } else if(old!=matches_.end())old->second=target->second;
        else if(matches_.size()<config.maximum)matches_.emplace(position,target->second);
    }
    void tick(const bedrock::LevelTickEvent& event) {
        if(stopped_.load())return;
        const auto config=config_.load();
        if(!config || !config->enabled || config->targets.empty()) { prepared_.store(nullptr); return; }
        const auto epoch=epoch_.load();
        auto scope=world_.tick(client_.load(),event.level,bedrock::WorldEpoch{epoch},
            bedrock::TickBudget{config->reads,std::chrono::microseconds{config->budgetUs}});
        if(!scope) { prepared_.store(nullptr); return; }
        const auto player=*world_.localPosition(); const auto generation=*world_.worldGeneration();
        if(generation!=generation_ || config->revision!=scanRevision_) {
            generation_=generation; scanRevision_=config->revision; scene_.fetch_add(1); prepared_.store(nullptr);
            drawDisabled_.store(false); restart(player,*config);
        }
        const Vec anchor{float(anchor_.x),float(anchor_.y),float(anchor_.z)};
        if(distance(anchor,player)>std::pow(std::max(2.f,config->range*.5f),2))restart(player,*config);
        const auto scene=scene_.load(); const auto deadline=Clock::now()+std::chrono::microseconds{config->budgetUs};
        unsigned reads=0;
        for(unsigned attempts=0;attempts<config->reads*8 && reads<config->reads && Clock::now()<deadline;++attempts) {
            // Recheck existing hits throughout the sweep instead of waiting for
            // the complete volume. Three new cells per one cached-hit sample.
            const bool checking=!matches_.empty() && ((schedule_++&3)==0);
            Pos position;
            if(checking) {
                if(check_==matches_.end())check_=matches_.begin();
                position=check_->first;
            } else {
                if(!pending_) {
                    auto position=cursor_.next();
                    if(!position && cursor_.finished()) { cursor_.rewind(); position=cursor_.next(); ++sweeps_; }
                    if(!position)continue; // retry within this tick's remaining time/attempt budget
                    pending_=*position;
                }
                position=*pending_;
                if(!inside(position,player,config->range)) { pending_.reset(); continue; }
            }
            const auto scan=world_.scanBlock(position);
            const auto& result=scan.block;
            if(result.error==bedrock::BlockError::budgetExceeded)break;
            ++reads;
            if(checking)++check_; else {
                pending_.reset();
                if(scan.emptySection||scan.unavailableSection) { cursor_.skipSection(); ++skippedSections_; }
            }
            update(position,result,*config,player);
        }
        if(epoch_.load()!=epoch || scene_.load()!=scene)return;
        if(Clock::now()>=nextPublish_) {
            std::unique_lock lock(mailMutex_,std::try_to_lock);
            if(lock) {
                mail_.scene=scene; mail_.player=player; mail_.config=config; mail_.count=0; mail_.captured=Clock::now();
                for(const auto& [position,target]:matches_) if(inside(position,player,config->range))mail_.matches[mail_.count++]={position,target};
                mailReady_=true; nextPublish_=Clock::now()+100ms;
            }
        }
        if(config->debug && Clock::now()>=nextDiagnostic_) {
            nextDiagnostic_=Clock::now()+5s;
            note("scan: hits="+std::to_string(matches_.size())+" reads_this_tick="+std::to_string(reads)+" skipped_sections="+std::to_string(skippedSections_)+" completed_sweeps="+std::to_string(sweeps_));
        }
    }
    struct Font { std::shared_ptr<const bedrock::world_text::NameMesh> mesh; Clock::time_point retry{}; };
    std::shared_ptr<const Prepared> prepare(const Capture& capture,bedrock::world_text::NameMesher& mesher) {
        auto output=std::make_shared<Prepared>(); output->scene=capture.scene; output->revision=capture.config->revision; output->captured=capture.captured;
        auto& geometry=output->geometry; geometry.origin=capture.player;
        bool cameraReady=false;
        { std::lock_guard lock(originMutex_); if(originScene_==capture.scene) { geometry.origin=origin_; cameraReady=true; } }
        std::vector<Match> matches(capture.matches.begin(),capture.matches.begin()+capture.count);
        std::sort(matches.begin(),matches.end(),[&](const Match& a,const Match& b) { return distance(center(a.position),capture.player)<distance(center(b.position),capture.player); });
        std::map<std::uint32_t,std::size_t> groups;
        unsigned labels=0;
        constexpr unsigned edges[12][2]={{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
        for(const auto& match:matches) {
            if(match.target>=capture.config->targets.size())continue;
            const auto& target=capture.config->targets[match.target]; const auto p=match.position;
            const float x=float(p.x),y=float(p.y),z=float(p.z),e=.002f;
            const std::array<Vec,8> corners{{{x-e,y-e,z-e},{x+1+e,y-e,z-e},{x+1+e,y-e,z+1+e},{x-e,y-e,z+1+e},
                {x-e,y+1+e,z-e},{x+1+e,y+1+e,z-e},{x+1+e,y+1+e,z+1+e},{x-e,y+1+e,z+1+e}}};
            auto [group,inserted]=groups.emplace(target.color,geometry.lines.size());
            if(inserted)geometry.lines.push_back({{((target.color>>16)&255)/255.f,((target.color>>8)&255)/255.f,(target.color&255)/255.f,1.f},{}});
            for(const auto& edge:edges)for(const auto n:edge) {
                const auto v=corners[n]; geometry.lines[group->second].vertices.push_back({v.x-geometry.origin.x,v.y-geometry.origin.y,v.z-geometry.origin.z});
            }
            if(!capture.config->showNames || !cameraReady || labels>=capture.config->labels)continue;
            auto& font=fonts_[target.name];
            if(!font.mesh && Clock::now()>=font.retry) {
                font.mesh=mesher.make(target.name);
                if(!font.mesh) { font.retry=Clock::now()+5s; note("name geometry deferred: "+target.name+" stage="+mesher.lastStage()); }
            }
            if(!font.mesh)continue;
            if(font.mesh->quads.size()*8>maximumTextVertices-geometry.textQuads.size())continue;
            const Vec anchor{x+.5f,y+1.2f,z+.5f};
            Vec direction{geometry.origin.x-anchor.x,geometry.origin.y-anchor.y,geometry.origin.z-anchor.z};
            const auto horizontal=std::hypot(direction.x,direction.z),length=std::hypot(horizontal,direction.y);
            Vec right{1,0,0},up{0,1,0};
            if(horizontal>.0001f && length>.0001f) {
                right={direction.z/horizontal,0,-direction.x/horizontal};
                direction={direction.x/length,direction.y/length,direction.z/length};
                up={direction.y*right.z,direction.z*right.x-direction.x*right.z,-direction.y*right.x};
            }
            for(const auto& quad:font.mesh->quads) {
                std::array<Vec,4> points;
                for(unsigned i=0;i<4;++i)points[i]={anchor.x+(right.x*quad[i].x+up.x*quad[i].y)*capture.config->nameScale-geometry.origin.x,
                    anchor.y+(right.y*quad[i].x+up.y*quad[i].y)*capture.config->nameScale-geometry.origin.y,
                    anchor.z+(right.z*quad[i].x+up.z*quad[i].y)*capture.config->nameScale-geometry.origin.z};
                for(const auto n:{0u,1u,2u,3u,3u,2u,1u,0u})geometry.textQuads.push_back(points[n]);
            }
            ++labels;
        }
        return output;
    }
    static void BDS_HOOK_CALL renderCallback(void* context,void* raw,BDS_HookNext next,void* frame) noexcept {
        next(frame,raw);
        auto& self=*static_cast<BlockOutline*>(context);
        if(self.stopped_.load())return;
        static thread_local bool rendering=false; if(rendering)return;
        rendering=true;
        try { self.render(*static_cast<BDS_RenderLevelArgs*>(raw)); }
        catch(...) { self.drawDisabled_.store(true); self.renderFault_.store(true); }
        rendering=false;
    }
    void render(const BDS_RenderLevelArgs& args) {
        const auto config=config_.load();
        if(!config || !config->enabled || config->targets.empty())return;
        const auto scene=scene_.load();
        if(const auto origin=renderer_.renderOrigin(args.renderer)) {
            std::unique_lock lock(originMutex_,std::try_to_lock);
            if(lock) { origin_=*origin; originScene_=scene; }
        }
        const auto snapshot=prepared_.load();
        if(drawDisabled_.load() || !config || !config->enabled || !snapshot || snapshot->scene!=scene ||
            snapshot->revision!=config->revision || Clock::now()-snapshot->captured>500ms)return;
        if(renderer_.draw(args.renderer,args.screen,snapshot->geometry)==bedrock::OutlineDraw::Fault) {
            drawDisabled_.store(true); renderFault_.store(true);
        }
    }
    bedrock::GameApi api_;
    bedrock::NativeWorld world_;
    bedrock::NativeOutlineRenderer renderer_;
    std::unique_ptr<bedrock::GameHooks> hooks_;
    BDS_HookToken renderToken_{};
    std::atomic<void*> client_{};
    std::atomic<bool> stopped_{},drawDisabled_{},renderFault_{};
    std::atomic<std::uint64_t> epoch_{1},scene_{1};
    std::atomic<std::shared_ptr<const bo::Config>> config_;
    std::atomic<std::shared_ptr<const Prepared>> prepared_;
    std::filesystem::path dir_; std::filesystem::file_time_type modified_{}; std::ofstream out_;
    std::mutex mailMutex_,originMutex_,logMutex_;
    Capture mail_; bool mailReady_{}; Vec origin_{}; std::uint64_t originScene_{};
    std::vector<std::string> logs_;
    std::map<std::string,Font> fonts_;
    std::map<Pos,unsigned> matches_;
    std::map<Pos,unsigned>::iterator check_=matches_.end();
    bo::ScanCursor cursor_; Pos anchor_{}; std::optional<Pos> pending_;
    std::uint64_t generation_{},scanRevision_{},configRevision_{},sweeps_{},skippedSections_{}; unsigned schedule_{};
    Clock::time_point nextPublish_{},nextConfig_{},nextDiagnostic_{};
};
BlockOutline mod;
DWORD WINAPI start(void*) {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&start),&module);
    mod.run(); return 0;
}
} // namespace
extern "C" __declspec(dllexport) unsigned BlockOutline_GetStatus() { return mod.status.load(); }
extern "C" __declspec(dllexport) void BlockOutline_Stop() { mod.stop(); }
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if(const auto thread=CreateThread(nullptr,0,start,nullptr,0,nullptr))CloseHandle(thread); else mod.status=4;
    } else if(reason==DLL_PROCESS_DETACH)mod.signalStop();
    return TRUE;
}
#endif
