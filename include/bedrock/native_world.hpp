#pragma once
#include <bedrock/detail/native_world_access.hpp>
#include <bedrock/resolver.hpp>
#include <chrono>
#include <thread>
#include <map>

namespace bedrock {
// Opt-in native adapter: no hooks or worker thread are installed. Bindings remain
// candidates; caller must explicitly opt in and provide an exact live profile.
class NativeWorld final : public BlockAccess {
public:
    struct Budget { unsigned operations=256; unsigned microseconds=500; };
    struct Capabilities { bool readBlocks{}, placeBlocks{}; bool candidate=true; };
    NativeWorld():reader_(bindings_){}
    NativeWorld(const NativeWorld&)=delete;
    NativeWorld& operator=(const NativeWorld&)=delete;
    bool load(const std::string& profile, bool allowCandidates, std::string& error) {
        ready_=false;
        if(!allowCandidates){error="native world candidate requires explicit opt-in";return false;}
        try {
            Resolver resolver;
            if(!resolver.loadJson(profile,error)||!resolver.attachWindowsMainModule(error)||!resolver.exactBuild()){
                if(error.empty())error="native world requires exact executable identity";
                return false;
            }
            bindings_.loadWorld(profile);reader_.clear();ready_=true;error.clear();return true;
        }catch(const std::exception& e){error=e.what();return false;}
    }
    Capabilities capabilities()const noexcept{return {ready_,ready_&&bindings_.placementReady,true};}
    // Construct ONLY inside the local client LevelTick callback. Nested scopes
    // are denied. Destruction revokes all native access until the next tick.
    class TickScope {
        NativeWorld* api_{};
    public:
        TickScope(NativeWorld& api,void* client,void* level,std::uint64_t epoch,Budget budget)
            :api_(api.enter(client,level,epoch,budget)?&api:nullptr){}
        ~TickScope(){if(api_)api_->leave();}
        TickScope(const TickScope&)=delete;
        TickScope& operator=(const TickScope&)=delete;
        explicit operator bool()const noexcept{return api_!=nullptr;}
    };
    TickScope tick(void* client,void* level,std::uint64_t epoch,Budget budget){return TickScope(*this,client,level,epoch,budget);}
    TickScope tick(void* client,void* level,std::uint64_t epoch){return tick(client,level,epoch,Budget{});}
    BlockResult getBlock(BlockPos pos,bool fresh=false){if(!onOwnerThread())return {{},BlockError::wrongThread};return worldBlock(level_,pos,fresh);}
    PlacementResult placeBlock(const BlockPlacement& request){if(!onOwnerThread())return {BlockError::wrongThread};return worldPlaceBlock(level_,request);}
    // setBlock is an inventory-backed placement, not a local terrain mutation.
    // Unsupported removal/replacement must use a future dedicated capability.
    PlacementResult setBlock(const BlockPlacement& request){return placeBlock(request);}
    PlacementResult setBlock(BlockPos target,std::string_view identifier,const BlockSetOptions& options={}){
        if(!onOwnerThread())return {BlockError::wrongThread};
        return worldSetBlock(level_,target,identifier,options);
    }
    PlacementResult worldSetBlock(void* world,BlockPos target,std::string_view identifier,const BlockSetOptions& options) override {
        auto error=check(world);if(error!=BlockError::none)return {error};
        if(!bindings_.placementReady)return {BlockError::unsupported};
        if(identifier.empty()||identifier=="minecraft:air"||(options.hotbarSlot&&*options.hotbarSlot>8))return {BlockError::invalidRequest};
        if(!consume(10))return {BlockError::budgetExceeded};
        world_native::NativeHotbar hotbar;if(!reader_.hotbar(player_,hotbar))return {BlockError::unavailable};
        unsigned slot=9;
        for(unsigned i=0;i<9;++i)if((!options.hotbarSlot||*options.hotbarSlot==i)&&hotbar.copied[i].count&&hotbar.copied[i].identifier==identifier){slot=i;break;}
        if(slot==9)return {BlockError::missingItem};
        auto* rotation=bindings_.component(player_,bindings_.actors.actorRotationComponentHash);float yaw{},pitch{};
        if(!world_native::read(rotation,bindings_.actors.rotationYawOffset,yaw)||!world_native::read(rotation,bindings_.actors.rotationPitchOffset,pitch))return {BlockError::unavailable};
        constexpr BlockPos normals[]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
        PlacementResult last{BlockError::unavailable};
        for(unsigned face:{1u,2u,3u,4u,5u,0u}){
            const auto n=normals[face];
            const std::int64_t x=std::int64_t(target.x)-n.x,y=std::int64_t(target.y)-n.y,z=std::int64_t(target.z)-n.z;
            if(x<INT32_MIN||x>INT32_MAX||y<INT32_MIN||y>INT32_MAX||z<INT32_MIN||z>INT32_MAX)continue;
            BlockPlacement request;request.target=target;request.support={int(x),int(y),int(z)};request.face=Face(face);request.hotbarSlot=slot;
            request.yaw=options.yaw.value_or(yaw);request.pitch=options.pitch.value_or(pitch);request.maxReach=options.maxReach;
            request.expectedProperties=options.expectedProperties;
            request.hitX=.5f+n.x*.5f;request.hitY=.5f+n.y*.5f;request.hitZ=.5f+n.z*.5f;
            last=worldPlaceBlock(world,request);
            if(last.dispatched||last.error==BlockError::budgetExceeded||last.error==BlockError::staleWorld)return last;
        }
        return last;
    }
    void* handle()const noexcept{return onOwnerThread()&&active_?level_:nullptr;}
    BlockResult worldBlock(void* world,BlockPos pos,bool fresh=false) override {
        auto error=check(world);if(error!=BlockError::none)return {{},error};
        if(!fresh){auto it=cache_.find(pos);if(it!=cache_.end())return it->second;}
        if(!consume())return {{},BlockError::budgetExceeded};
        auto cell=reader_.getBlock(source_,pos);
        BlockResult result{std::move(cell),BlockError::none};
        if(result.block.kind==CellKind::unknown)result.error=BlockError::unavailable;
        cache_.insert_or_assign(pos,result);return result;
    }
    PlacementResult worldPlaceBlock(void* world,const BlockPlacement& q) override {
        auto error=check(world);if(error!=BlockError::none)return {error};
        if(!bindings_.placementReady)return {BlockError::unsupported};
        const auto face=static_cast<unsigned>(q.face);
        constexpr BlockPos normal[]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
        if(face>5||q.hotbarSlot>8||!std::isfinite(q.yaw)||!std::isfinite(q.pitch)||q.pitch < -90||q.pitch>90||
           !std::isfinite(q.maxReach)||q.maxReach<=0||q.maxReach>16||
           !unit(q.hitX)||!unit(q.hitY)||!unit(q.hitZ))return {BlockError::invalidRequest};
        const auto n=normal[face];
        if(std::int64_t(q.support.x)+n.x!=q.target.x||std::int64_t(q.support.y)+n.y!=q.target.y||std::int64_t(q.support.z)+n.z!=q.target.z)
            return {BlockError::invalidRequest};
        auto target=getBlock(q.target,true),support=getBlock(q.support,true);
        if(!target)return {target.error};if(!support)return {support.error};
        if((target.block.kind!=CellKind::air&&!q.allowAugment)||support.block.kind!=CellKind::block)return {BlockError::invalidRequest};
        // Reserve bounded native work before any rotation/selection/placement write.
        if(!consume(16))return {BlockError::budgetExceeded};
        Vec3 position{};std::uint64_t runtime{};
        if(!bindings_.pose(player_,position,runtime)||runtime!=runtime_)return {BlockError::staleWorld};
        ActorBounds bounds{};auto* shape=bindings_.component(player_,bindings_.actors.aabbShapeComponentHash);
        if(!world_native::read(shape,bindings_.actors.aabbMinimumOffset,bounds.minimum)||
           !world_native::read(shape,bindings_.actors.aabbMaximumOffset,bounds.maximum)||!bounds.valid())return {BlockError::unavailable};
        const Vec3 eye{position.x,bounds.maximum.y-.1f,position.z};
        const Vec3 hit{float(q.support.x)+q.hitX,float(q.support.y)+q.hitY,float(q.support.z)+q.hitZ};
        const double dx=hit.x-eye.x,dy=hit.y-eye.y,dz=hit.z-eye.z;
        if(dx*dx+dy*dy+dz*dz>q.maxReach*q.maxReach)return {BlockError::invalidRequest};
        world_native::NativeHotbar hotbar;
        if(!reader_.hotbar(player_,hotbar))return {BlockError::unavailable};
        if(!hotbar.copied[q.hotbarSlot].count||!hotbar.types[q.hotbarSlot])return {BlockError::missingItem};
        void* predicted{};
        if(!bindings_.predict(hotbar.types[q.hotbarSlot],player_,q.target,face,{q.hitX,q.hitY,q.hitZ},hotbar.aux[q.hotbarSlot],q.yaw,q.pitch,&predicted))return {BlockError::unavailable};
        auto prediction=reader_.describeCell(predicted);
        if(prediction.kind!=CellKind::block||prediction.identifier!=hotbar.copied[q.hotbarSlot].identifier||
           (q.expectedProperties&&(!prediction.properties||prediction.properties->values!=q.expectedProperties->values)))return {BlockError::stateMismatch};
        auto* mode=bindings_.gameMode(player_);if(!mode)return {BlockError::unavailable};
        auto* rotation=bindings_.component(player_,bindings_.actors.actorRotationComponentHash);
        float yaw{},pitch{};
        if(!world_native::read(rotation,bindings_.actors.rotationYawOffset,yaw)||!world_native::read(rotation,bindings_.actors.rotationPitchOffset,pitch))return {BlockError::unavailable};
        const int slot=bindings_.selected(player_);if(slot<0||slot>8)return {BlockError::unavailable};
        bool accepted=false,called=false;
        const bool selected=bindings_.select(player_,q.hotbarSlot);
        if(selected&&world_native::write(rotation,bindings_.actors.rotationYawOffset,q.yaw)&&world_native::write(rotation,bindings_.actors.rotationPitchOffset,q.pitch))
            called=bindings_.build(player_,mode,q.support,face,eye,hit,&accepted);
        world_native::write(rotation,bindings_.actors.rotationYawOffset,yaw);
        world_native::write(rotation,bindings_.actors.rotationPitchOffset,pitch);
        bindings_.select(player_,unsigned(slot));cache_.erase(q.target);
        return {called&&accepted?BlockError::none:BlockError::nativeRejected,called,accepted};
    }
private:
    world_native::Bindings bindings_;
    world_native::Reader reader_;
    bool ready_{},active_{};
    std::atomic<DWORD> owner_{0};
    bool onOwnerThread()const noexcept{return owner_.load(std::memory_order_acquire)==GetCurrentThreadId();}
    void* level_{};void* source_{};void* player_{};void* lastLevel_{};void* lastSource_{};
    std::uint64_t epoch_{},runtime_{},lastEpoch_{},lastRuntime_{};
    unsigned remaining_{};
    std::chrono::steady_clock::time_point deadline_;
    std::map<BlockPos,BlockResult> cache_;
    static bool unit(float v){return std::isfinite(v)&&v>=0&&v<=1;}
    BlockError check(void* world)const {
        if(!onOwnerThread())return BlockError::wrongThread;
        if(!ready_)return BlockError::unsupported;
        if(!active_||!world||world!=level_)return BlockError::staleWorld;
        return BlockError::none;
    }
    bool consume(unsigned n=1){if(n>remaining_||std::chrono::steady_clock::now()>=deadline_)return false;remaining_-=n;return true;}
    bool enter(void* client,void* level,std::uint64_t epoch,Budget budget){
        const DWORD id=GetCurrentThreadId();DWORD expected=0;
        owner_.compare_exchange_strong(expected,id,std::memory_order_acq_rel);
        if(!onOwnerThread()||!ready_||active_||!client||!level||!budget.operations||!budget.microseconds)return false;
        auto* player=bindings_.local(client);void* actualLevel{};
        auto* source=bindings_.region(client);Vec3 position{};std::uint64_t runtime{};
        if(!source||!bindings_.pose(player,position,runtime)||!world_native::read(player,bindings_.layout("player_level_offset"),actualLevel)||actualLevel!=level)return false;
        if(lastLevel_!=level||lastSource_!=source||lastEpoch_!=epoch||lastRuntime_!=runtime)reader_.clear();
        lastLevel_=level;lastSource_=source;lastEpoch_=epoch;lastRuntime_=runtime;
        level_=level;source_=source;player_=player;runtime_=runtime;epoch_=epoch;
        remaining_=std::min(budget.operations,4096u);deadline_=std::chrono::steady_clock::now()+std::chrono::microseconds(std::min(budget.microseconds,5000u));
        cache_.clear();active_=true;return true;
    }
    void leave(){active_=false;level_=source_=player_=nullptr;cache_.clear();}
};
} // namespace bedrock
