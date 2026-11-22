#include <bedrock/sdk/world_access.hpp>

#ifdef _WIN32
#include <utility>
#endif

#ifdef _WIN32
#include <bedrock/native/world_access.hpp>
#endif

namespace bedrock::sdk {
struct WorldAccess::Impl {
#ifdef _WIN32
    bedrock::NativeWorldAccess world;
#endif
};
WorldAccess::WorldAccess():impl_(std::make_unique<Impl>()){}
WorldAccess::~WorldAccess()=default;
#ifdef _WIN32
void WorldAccess::load(const std::string& profileJson){impl_->world.load(profileJson);} 
void WorldAccess::loadWorld(const std::string& profileJson){impl_->world.loadWorld(profileJson);} 
bool WorldAccess::placementReady()const noexcept{return impl_->world.placementReady();}
std::uint64_t WorldAccess::layout(std::string_view name)const{return impl_->world.layout(name);}
void WorldAccess::reserveCaches(std::size_t states,std::size_t names){impl_->world.reserveCaches(states,names);} 
void WorldAccess::clear(){impl_->world.clear();}
bool WorldAccess::readField(void* object,std::size_t offset,float& value)const noexcept{return impl_->world.readField(object,offset,value);}
bool WorldAccess::readField(void* object,std::size_t offset,std::uint16_t& value)const noexcept{return impl_->world.readField(object,offset,value);}
bool WorldAccess::readField(void* object,std::size_t offset,std::uint64_t& value)const noexcept{return impl_->world.readField(object,offset,value);}
bool WorldAccess::readField(void* object,std::size_t offset,void*& value)const noexcept{return impl_->world.readField(object,offset,value);}
bool WorldAccess::build(void* player,void* gameMode,BlockPos support,unsigned face,Vec3Value eye,Vec3Value hit,bool* accepted)const noexcept{return impl_->world.build(player,gameMode,bedrock::BlockPos{support.x,support.y,support.z},face,bedrock::Vec3{eye.x,eye.y,eye.z},bedrock::Vec3{hit.x,hit.y,hit.z},accepted);}
bool WorldAccess::writeField(void* object,std::size_t offset,float value)noexcept{return impl_->world.writeField(object,offset,value);}
bool WorldAccess::readField(void* object,std::string_view layout,float& value)const noexcept{return impl_->world.readField(object,impl_->world.layout(layout),value);}
bool WorldAccess::readField(void* object,std::string_view layout,std::uint16_t& value)const noexcept{return impl_->world.readField(object,impl_->world.layout(layout),value);}
bool WorldAccess::readField(void* object,std::string_view layout,std::uint64_t& value)const noexcept{return impl_->world.readField(object,impl_->world.layout(layout),value);}
bool WorldAccess::writeField(void* object,std::string_view layout,float value)noexcept{return impl_->world.writeField(object,impl_->world.layout(layout),value);}
void* WorldAccess::localPlayer(void* client)const noexcept{return impl_->world.localPlayer(client);} 
void* WorldAccess::region(void* client)const noexcept{return impl_->world.region(client);} 
bool WorldAccess::pose(void* player,Vec3Value& position,std::uint64_t& runtime)const noexcept{bedrock::Vec3 v{};bool ok=impl_->world.pose(player,v,runtime);position={v.x,v.y,v.z};return ok;} 
void* WorldAccess::gameMode(void* player)const noexcept{return impl_->world.gameMode(player);} 
void* WorldAccess::component(void* actor,std::uint32_t hash)const noexcept{return impl_->world.component(actor,hash);} 
int WorldAccess::selected(void* player)const noexcept{return impl_->world.selected(player);} 
bool WorldAccess::select(void* player,unsigned slot)noexcept{return impl_->world.select(player,slot);} 
bool WorldAccess::hotbar(void* player,HotbarSnapshot& snapshot)const noexcept{bedrock::NativeHotbar hotbar{};if(!impl_->world.hotbar(player,hotbar))return false;for(unsigned i=0;i<9;++i){snapshot.types[i]=hotbar.types[i];snapshot.aux[i]=hotbar.aux[i];snapshot.copied[i]={hotbar.copied[i].identifier,hotbar.copied[i].count};}return true;} 
ActorLayoutValue WorldAccess::actors() const noexcept { const auto& a=impl_->world.actors(); return {a.actorRotationComponentHash,a.rotationPitchOffset,a.rotationYawOffset,a.aabbShapeComponentHash,a.aabbShapeComponentHashAlternate,a.aabbMinimumOffset,a.aabbMaximumOffset}; }
bool WorldAccess::readField(void* object,std::size_t offset,Vec3Value& value)const noexcept{bedrock::Vec3 v{};if(!impl_->world.readField(object,offset,v))return false;value={v.x,v.y,v.z};return true;}
Cell WorldAccess::getBlock(void* source,BlockPos pos,bool inspectSupport) const { bedrock::BlockPos p{pos.x,pos.y,pos.z}; bedrock::Cell n=impl_->world.getBlock(source,p,inspectSupport); Cell out; out.position={n.position.x,n.position.y,n.position.z}; out.identifier=n.identifier; out.stateIdentity=n.stateIdentity; out.kind=static_cast<CellKind>(static_cast<unsigned>(n.kind)); if(n.properties){auto publicProperties=std::make_shared<BlockProperties>();for(const auto& [key,value]:n.properties->values)publicProperties->values.emplace(key,value);out.properties=std::move(publicProperties);} out.supportsPlacement=n.supportsPlacement; out.nativePlacementCandidate=n.nativePlacementCandidate; out.legacyData=n.legacyData; if(n.placement){PlacementRecipe r;r.itemIdentifier=n.placement->itemIdentifier;r.face=static_cast<Face>(static_cast<unsigned>(n.placement->face));r.yaw=n.placement->yaw;r.pitch=n.placement->pitch;r.hitX=n.placement->hitX;r.hitY=n.placement->hitY;r.hitZ=n.placement->hitZ;r.deferredState=n.placement->deferredState;r.augment=n.placement->augment;r.orientationKnown=n.placement->orientationKnown;r.exactStateSupported=n.placement->exactStateSupported;out.placement=r;} return out; }
Cell WorldAccess::describeCell(void* state,BlockPos pos) const { bedrock::BlockPos p{pos.x,pos.y,pos.z}; bedrock::Cell n=impl_->world.describeCell(state,p); Cell out; out.position={n.position.x,n.position.y,n.position.z}; out.identifier=n.identifier; out.stateIdentity=n.stateIdentity; out.kind=static_cast<CellKind>(static_cast<unsigned>(n.kind)); if(n.properties){auto publicProperties=std::make_shared<BlockProperties>();for(const auto& [key,value]:n.properties->values)publicProperties->values.emplace(key,value);out.properties=std::move(publicProperties);} out.supportsPlacement=n.supportsPlacement; out.nativePlacementCandidate=n.nativePlacementCandidate; out.legacyData=n.legacyData; if(n.placement){PlacementRecipe r;r.itemIdentifier=n.placement->itemIdentifier;r.face=static_cast<Face>(static_cast<unsigned>(n.placement->face));r.yaw=n.placement->yaw;r.pitch=n.placement->pitch;r.hitX=n.placement->hitX;r.hitY=n.placement->hitY;r.hitZ=n.placement->hitZ;r.deferredState=n.placement->deferredState;r.augment=n.placement->augment;r.orientationKnown=n.placement->orientationKnown;r.exactStateSupported=n.placement->exactStateSupported;out.placement=r;} return out; }
bool WorldAccess::predict(void* type,void* player,BlockPositionValue target,unsigned face,Vec3Value fractional,int aux,float yaw,float pitch,void*& result)const noexcept{bedrock::BlockPos p{target.x,target.y,target.z};bedrock::Vec3 f{fractional.x,fractional.y,fractional.z};return impl_->world.predict(type,player,p,face,f,aux,yaw,pitch,&result);} 
const BlockInfoValue* WorldAccess::describe(void*){return nullptr;} std::string WorldAccess::typeName(void*)const{return {};}
#else
ActorLayoutValue WorldAccess::actors() const noexcept { return {}; }
Cell WorldAccess::getBlock(void*,BlockPos,bool) const { return {}; }
Cell WorldAccess::describeCell(void*,BlockPos) const { return {}; }
bool WorldAccess::readField(void*,std::size_t,float&)const noexcept{return false;}
bool WorldAccess::readField(void*,std::size_t,std::uint16_t&)const noexcept{return false;}
bool WorldAccess::readField(void*,std::size_t,std::uint64_t&)const noexcept{return false;}
bool WorldAccess::readField(void*,std::size_t,void*& )const noexcept{return false;}
bool WorldAccess::readField(void*,std::size_t,Vec3Value&)const noexcept{return false;}
bool WorldAccess::build(void*,void*,BlockPos,unsigned,Vec3Value,Vec3Value,bool*)const noexcept{return false;}
bool WorldAccess::writeField(void*,std::size_t,float)noexcept{return false;}
void WorldAccess::load(const std::string&){} void WorldAccess::loadWorld(const std::string&){} bool WorldAccess::placementReady()const noexcept{return false;} std::uint64_t WorldAccess::layout(std::string_view)const{return 0;} void WorldAccess::reserveCaches(std::size_t,std::size_t){} void WorldAccess::clear(){} bool WorldAccess::readField(void*,std::string_view,std::uint64_t&)const noexcept{return false;} bool WorldAccess::readField(void*,std::string_view,std::uint16_t&)const noexcept{return false;} bool WorldAccess::readField(void*,std::string_view,float&)const noexcept{return false;} bool WorldAccess::writeField(void*,std::string_view,float)noexcept{return false;} void* WorldAccess::localPlayer(void*)const noexcept{return nullptr;} void* WorldAccess::region(void*)const noexcept{return nullptr;} bool WorldAccess::pose(void*,Vec3Value&,std::uint64_t&)const noexcept{return false;} void* WorldAccess::gameMode(void*)const noexcept{return nullptr;} void* WorldAccess::component(void*,std::uint32_t)const noexcept{return nullptr;} int WorldAccess::selected(void*)const noexcept{return -1;} bool WorldAccess::select(void*,unsigned)noexcept{return false;} bool WorldAccess::hotbar(void*,HotbarSnapshot&)const noexcept{return false;} const BlockInfoValue* WorldAccess::describe(void*){return nullptr;} std::string WorldAccess::typeName(void*)const{return {};} bool WorldAccess::predict(void*,void*,BlockPositionValue,unsigned,Vec3Value,int,float,float,void*& result)const noexcept{result=nullptr;return false;}
#endif
namespace detail {
void* nativeWorldAccessHandle(WorldAccess& access) noexcept {
#ifdef _WIN32
    return &access.impl_->world;
#else
    (void)access;
    return nullptr;
#endif
}
} // namespace detail
} // namespace bedrock::sdk
