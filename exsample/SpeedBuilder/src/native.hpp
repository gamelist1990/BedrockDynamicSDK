#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bedrock/native_actors.hpp>
#include <bedrock/native_items.hpp>
#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include "speed_builder/model.hpp"
#include <unordered_map>
#include <cstring>

namespace speedbuilder_native {
struct Entity{std::uint32_t value;constexpr operator std::uint32_t()const{return value;}constexpr bool operator==(const Entity&)const=default;};
struct Traits{using value_type=Entity;using entity_type=std::uint32_t;using version_type=std::uint16_t;
    static constexpr entity_type entity_mask=0x3ffff;static constexpr version_type version_mask=0x3fff;};
}
namespace entt{template<>struct entt_traits<speedbuilder_native::Entity>:basic_entt_traits<speedbuilder_native::Traits>{static constexpr std::size_t page_size=2048;};}
namespace speedbuilder_native {
using Pos=speed_builder::BlockPos;using Vec=bedrock::Vec3;
template<class T>bool read(const void* ptr,std::size_t offset,T& out)noexcept{
    if(!ptr||reinterpret_cast<std::uintptr_t>(ptr)<0x10000)return false;
    __try{std::memcpy(&out,static_cast<const std::byte*>(ptr)+offset,sizeof(T));return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
template<class T>bool write(void* ptr,std::size_t offset,const T& value)noexcept{
    if(!ptr)return false;
    __try{std::memcpy(static_cast<std::byte*>(ptr)+offset,&value,sizeof(T));return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
struct StringCopy{char bytes[2049]{};std::size_t size{};};
inline bool copyString(const void* value,StringCopy* out)noexcept{
    if(!value||!out)return false;
    __try{auto* p=static_cast<const std::byte*>(value);std::size_t size{},capacity{};
        std::memcpy(&size,p+16,8);std::memcpy(&capacity,p+24,8);
        if(size>2048||capacity<size||capacity>0x1000000)return false;
        const char* chars=reinterpret_cast<const char*>(value);
        if(capacity>15)std::memcpy(&chars,p,8);
        if(!chars)return false;std::memcpy(out->bytes,chars,size);out->bytes[size]=0;out->size=size;return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
inline std::uint32_t number(const nlohmann::json& j){auto n=j.is_string()?std::stoull(j.get<std::string>(),nullptr,0):j.get<std::uint64_t>();
    if(n>UINT32_MAX)throw std::runtime_error("native profile integer overflow");return static_cast<std::uint32_t>(n);}
struct Bindings{
    std::uintptr_t base{},textBegin{},textEnd{};
    bedrock::NativeActorLayout actors;bedrock::NativeItemLayout items;
    void* regionFn{};void* getBlockFn{};void* selectFn{};void* getHitFn{};
    void* titleReadFn{};std::uintptr_t titleVtable{};
    std::array<std::uintptr_t,2> gameTables{};
    bool executable(void* p)const noexcept{const auto n=reinterpret_cast<std::uintptr_t>(p);return n>=textBegin&&n<textEnd;}
    void* checked(const nlohmann::json& fn)const{
        auto* p=reinterpret_cast<void*>(base+number(fn.at("rva")));if(!executable(p))throw std::runtime_error("native target outside .text");
        const auto guard=fn.at("entry_check").get<std::string>();if(guard.size()<23||(guard.size()+1)%3)throw std::runtime_error("bad native guard");
        for(std::size_t i=0;i<guard.size();i+=3){unsigned char value{};if(!read(p,i/3,value)||value!=std::stoul(guard.substr(i,2),nullptr,16))throw std::runtime_error("native entry mismatch");}return p;
    }
    void load(const std::string& profile){const auto j=nlohmann::json::parse(profile);const auto& f=j.at("native_speed_builder");
        if(f.at("schema")!=1||f.value("revision",0)<3)throw std::runtime_error("unsupported SpeedBuilder schema");
        base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));textBegin=base+number(j.at("target").at("text_rva"));textEnd=textBegin+number(j.at("target").at("text_size"));
        std::string error;if(!bedrock::parseNativeActorLayout(profile,actors,error)||!bedrock::parseNativeItemLayout(profile,items,error))throw std::runtime_error(error);
        regionFn=checked(f.at("get_region"));getBlockFn=checked(f.at("get_block"));selectFn=checked(f.at("select_hotbar"));getHitFn=checked(f.at("get_hit_result"));
        titleReadFn=checked(f.at("title_read"));titleVtable=base+number(f.at("title_vtable_rva"));
        checked(f.at("build_block"));checked(f.at("base_placement_state"));
        for(unsigned i=0;i<2;++i)gameTables[i]=base+number(f.at("game_mode_vtables").at(i));
    }
    void* component(void* actor,std::uint32_t hash)const noexcept{
        void* registry{};std::uint32_t id{};
        if(!read(actor,actors.actorEntityContextOffset+actors.contextRegistryOffset,registry)||!read(actor,actors.actorEntityContextOffset+actors.contextEntityIdOffset,id)||!registry)return nullptr;
        __try{auto* r=static_cast<entt::basic_registry<Entity>*>(registry);Entity e{id};auto* s=r->storage(hash);return r->valid(e)&&s&&s->contains(e)?s->value(e):nullptr;}__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
    }
    void* local(void* client)const noexcept{
        void** table{};void* fn{};if(!read(client,0,table)||!read(table,items.localPlayerVtableSlot*8,fn)||!executable(fn))return nullptr;
        __try{return reinterpret_cast<void*(__fastcall*)(void*)>(fn)(client);}__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
    }
    void* region(void* client)const noexcept{
        if(!client)return nullptr;__try{return reinterpret_cast<void*(__fastcall*)(void*)>(regionFn)(client);}__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
    }
    bool pose(void* player,Vec& position,std::uint64_t& runtime)const noexcept{
        return read(component(player,actors.stateVectorComponentHash),actors.statePositionOffset,position)&&
            read(component(player,actors.runtimeIdComponentHash),actors.runtimeIdValueOffset,runtime)&&runtime&&
            std::isfinite(position.x)&&std::isfinite(position.y)&&std::isfinite(position.z)&&
            std::abs(position.x)<30000000&&std::abs(position.z)<30000000&&std::abs(position.y)<10000;
    }
    void* gameMode(void* player)const noexcept{
        void* gm{};std::uintptr_t vt{};void* owner{};
        if(!read(player,0xaa0,gm)||!read(gm,0,vt)||(vt!=gameTables[0]&&vt!=gameTables[1])||!read(gm,8,owner)||owner!=player)return nullptr;return gm;
    }
    void* block(void* source,const Pos& pos)const noexcept{
        void** vt{};void* fn{};void* chunkFn{};
        if(!read(source,0,vt)||!read(vt,16,fn)||fn!=getBlockFn||!read(vt,0x148,chunkFn)||!executable(chunkFn))return nullptr;
        const std::array<int,2> chunk{pos.x>>4,pos.z>>4};
        __try{if(!reinterpret_cast<void*(__fastcall*)(void*,const void*)>(chunkFn)(source,&chunk))return nullptr;
            return reinterpret_cast<void*(__fastcall*)(void*,const Pos*)>(fn)(source,&pos);}__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
    }
    // Same hidden-return ABI used by InsideCubeRenderer. Query only support
    // candidates, not the full preview volume. Non-solid slabs/chests can still
    // provide a usable collision surface.
    bool collision(void* type,void* state,void* source,const Pos& pos)const noexcept{
        void** vt{};void* fn{};
        if(!read(type,0,vt)||!read(vt,4*8,fn)||!executable(fn))return false;
        bedrock::ActorBounds box{};
        __try{
            reinterpret_cast<void(__fastcall*)(void*,void*,void*,void*,const Pos*,void*)>(fn)(type,&box,state,source,&pos,nullptr);
            const auto& a=box.minimum;const auto& z=box.maximum;
            return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z)&&std::isfinite(z.x)&&std::isfinite(z.y)&&std::isfinite(z.z)&&
                z.x>a.x&&z.y>a.y&&z.z>a.z&&z.x-a.x<64&&z.y-a.y<64&&z.z-a.z<64;
        }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    void* stack(void* player,unsigned slot)const noexcept{
        void* supplies{};void* inventory{};void** vt{};void* fn{};
        if(slot>=9||!read(player,items.localPlayerSuppliesOffset,supplies)||!read(supplies,items.suppliesInventoryOffset,inventory)||!read(inventory,0,vt)||!read(vt,items.inventoryGetItemVtableSlot*8,fn)||!executable(fn))return nullptr;
        __try{return reinterpret_cast<void*(__fastcall*)(void*,int)>(fn)(inventory,static_cast<int>(slot));}__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
    }
    int selected(void* player)const noexcept{void* supplies{};int slot=-1;if(read(player,items.localPlayerSuppliesOffset,supplies))read(supplies,items.suppliesSelectedSlotOffset,slot);return slot;}
    bool select(void* player,unsigned slot)const noexcept{
        void* supplies{};if(slot>8||!read(player,items.localPlayerSuppliesOffset,supplies)||!supplies)return false;
        __try{if(!reinterpret_cast<bool(__fastcall*)(void*,int,signed char)>(selectFn)(supplies,slot,0))return false;int selectedSlot=-1;return read(supplies,items.suppliesSelectedSlotOffset,selectedSlot)&&selectedSlot==static_cast<int>(slot);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    // Predictors run synchronously on the owning tick, restoring rotation on every exit.
    bool predict(void* type,void* player,const Pos& target,unsigned face,Vec fractional,int aux,float yaw,float pitch,void** result)const noexcept{
        auto* rot=component(player,actors.actorRotationComponentHash);float oldYaw{},oldPitch{};void** vt{};void* fn{};
        if(!read(rot,actors.rotationYawOffset,oldYaw)||!read(rot,actors.rotationPitchOffset,oldPitch)||!read(type,0,vt)||!read(vt,79*8,fn)||!executable(fn))return false;
        bool ok=false;
        __try{__try{if(write(rot,actors.rotationYawOffset,yaw)&&write(rot,actors.rotationPitchOffset,pitch)){
            *result=reinterpret_cast<void*(__fastcall*)(void*,void*,const Pos*,unsigned char,const Vec*,int)>(fn)(type,player,&target,static_cast<unsigned char>(face),&fractional,aux);ok=*result!=nullptr;
        }}__finally{write(rot,actors.rotationYawOffset,oldYaw);write(rot,actors.rotationPitchOffset,oldPitch);}}
        __except(EXCEPTION_EXECUTE_HANDLER){ok=false;}return ok;
    }
    struct HitHead{Vec start,direction;int type;unsigned char face,padding[3];Pos block;Vec hit;};
    static_assert(sizeof(HitHead)==0x38);
    bool build(void* player,void* gm,const Pos& support,unsigned face,Vec eye,Vec hit,bool* accepted)const noexcept{
        void* level{};void** vt{};void* getter{};void* head{};void** gameVt{};void* buildFn{};
        if(!read(player,0x1d8,level)||!read(level,0,vt)||!read(vt,0xa78,getter)||getter!=getHitFn||
           !read(gm,0,gameVt)||!read(gameVt,6*8,buildFn)||!executable(buildFn))return false;
        HitHead original{};bool saved=false,called=false;
        __try{__try{
            head=reinterpret_cast<void*(__fastcall*)(void*)>(getter)(level);if(!read(head,0,original))return false;saved=true;
            auto input=original;input.start=eye;input.direction={hit.x-eye.x,hit.y-eye.y,hit.z-eye.z};input.type=0;input.face=static_cast<unsigned char>(face);input.block=support;input.hit=hit;
            if(!write(head,0,input))return false;
            // New placement, not vanilla's drag-direction continuation.
            const bool no=false;if(!write(gm,0x40,no)||!write(gm,0x41,no))return false;
            *accepted=reinterpret_cast<bool(__fastcall*)(void*,const Pos*,unsigned char,unsigned char,bool)>(buildFn)(gm,&support,static_cast<unsigned char>(face),0,false);called=true;
        }__finally{if(saved)write(head,0,original);}}
        __except(EXCEPTION_EXECUTE_HANDLER){called=false;}return called;
    }
};
inline bool identifier(std::string_view name){if(name.empty()||name.size()>256||name.find(':')==name.npos)return false;
    return std::all_of(name.begin(),name.end(),[](unsigned char c){return c>32&&c<127;});}
// Bounded read-only view of the exact-build MSVC map/TagVariant layout.
// No foreign STL methods or native NBT constructors are called.
inline bool tagEntries(const void* compound,std::vector<std::pair<std::string,void*>>& entries){
    void* head{};std::uint64_t count{};unsigned char nil{};void* root{};
    if(!read(compound,8,head)||!read(compound,16,count)||count>128||!read(head,25,nil)||nil!=1||!read(head,8,root))return false;
    if(!count)return root==head;
    void* parent{};if(!root||root==head||!read(root,8,parent)||parent!=head)return false;
    std::array<void*,128> queue{};queue[0]=root;std::size_t used=1;
    for(std::size_t i=0;i<used;++i){auto* node=queue[i];unsigned char type{};StringCopy key;
        if(!read(node,25,nil)||nil||!copyString(static_cast<const std::byte*>(node)+0x20,&key)||key.size>128||!key.size||!read(node,0x68,type)||type>11)return false;
        std::string name(key.bytes,key.size);if(!std::all_of(name.begin(),name.end(),[](unsigned char c){return c>32&&c<127;}))return false;
        entries.emplace_back(std::move(name),static_cast<std::byte*>(node)+0x40);
        for(unsigned offset:{0u,16u}){void* child{};if(!read(node,offset,child)||!read(child,25,nil))return false;if(nil)continue;
            if(used>=count||used>=queue.size()||!read(child,8,parent)||parent!=node||std::find(queue.begin(),queue.begin()+used,child)!=queue.begin()+used)return false;
            queue[used++]=child;
        }
    }return used==count;
}
inline std::shared_ptr<const speed_builder::BlockProperties> properties(const Bindings& b,void* state,std::string_view expectedName){
    const auto* root=static_cast<const std::byte*>(state)+0xf0;void** vt{};void* idFn{};std::array<unsigned char,6> code{};
    if(!read(root,0,vt)||!read(vt,5*8,idFn)||!b.executable(idFn)||!read(idFn,0,code))return {};
    const bool compound=(code[0]==0xb0&&code[1]==10&&code[2]==0xc3)||(code[0]==0xb8&&code[1]==10&&code[2]==0&&code[3]==0&&code[4]==0&&code[5]==0xc3);
    if(!compound)return {};
    std::vector<std::pair<std::string,void*>> entries;if(!tagEntries(root,entries))return {};
    void* states{};bool nameMatched=false;
    for(const auto& [key,value]:entries){unsigned char type{};if(!read(value,0x28,type))return {};
        if(key=="name"&&type==8){StringCopy name;if(!copyString(static_cast<const std::byte*>(value)+8,&name))return {};nameMatched=std::string_view(name.bytes,name.size)==expectedName;}
        if(key=="states"&&type==10)states=value;
    }
    if(!states||!nameMatched)return {};
    entries.clear();if(!tagEntries(states,entries))return {};
    auto result=std::make_shared<speed_builder::BlockProperties>();
    for(const auto& [key,value]:entries){unsigned char type{};if(!read(value,0x28,type))return {};
        if(type==1){signed char n{};if(!read(value,8,n))return {};if(!result->values.emplace(key,int(n)).second)return {};}
        else if(type==3){int n{};if(!read(value,8,n))return {};if(!result->values.emplace(key,n).second)return {};}
        else if(type==8){StringCopy text;if(!copyString(static_cast<const std::byte*>(value)+8,&text)||text.size>256)return {};if(!result->values.emplace(key,std::string(text.bytes,text.size)).second)return {};}
        else return {};
    }return result;
}
struct BlockInfo{std::string name;void* type{};bool solid{};std::shared_ptr<const speed_builder::BlockProperties> properties;};
struct NativeHotbar{speed_builder::Hotbar copied{};std::array<void*,9> types{};std::array<int,9> aux{};};
class Reader{
public:
    Bindings& b;std::unordered_map<void*,BlockInfo> states;std::unordered_map<void*,std::string> names;
    explicit Reader(Bindings& bindings):b(bindings){}
    void clear(){states.clear();names.clear();}
    std::string typeName(void* type){if(auto it=names.find(type);it!=names.end())return it->second;
        StringCopy value;if(!copyString(static_cast<std::byte*>(type)+0xe8,&value))return {};
        std::string name(value.bytes,value.size);if(!identifier(name))return {};names.emplace(type,name);return name;}
    BlockInfo* describe(void* state){
        if(!state)return nullptr;auto it=states.find(state);if(it!=states.end())return &it->second;
        BlockInfo info;std::uint8_t flags{};if(!read(state,0x68,info.type)||!info.type)return nullptr;
        info.name=typeName(info.type);if(info.name.empty()||!read(info.type,0x164,flags))return nullptr;
        info.solid=(flags&2)!=0;info.properties=properties(b,state,info.name);
        return &states.emplace(state,std::move(info)).first->second;
    }
    speed_builder::Cell describeCell(void* state,Pos pos={}){
        speed_builder::Cell result;result.position=pos;auto* info=describe(state);if(!info)return result;
        result.identifier=info->name;result.stateIdentity=reinterpret_cast<std::uintptr_t>(state);result.properties=info->properties;
        if(info->name=="minecraft:air"||info->name=="minecraft:cave_air"||info->name=="minecraft:void_air")result.kind=speed_builder::CellKind::air;
        else{result.kind=speed_builder::CellKind::block;result.supportsPlacement=info->solid;result.nativePlacementCandidate=true;
            speed_builder::PlacementRecipe recipe;recipe.itemIdentifier=result.identifier;result.placement=recipe;}
        return result;
    }
    speed_builder::Cell cell(void* source,Pos pos,bool inspectSupport=false){
        auto* state=b.block(source,pos);auto result=describeCell(state,pos);
        if(inspectSupport&&result.kind==speed_builder::CellKind::block&&!result.supportsPlacement){auto* info=describe(state);result.supportsPlacement=info&&b.collision(info->type,state,source,pos);}
        return result;
    }
    bool hotbar(void* player,NativeHotbar& result){result={};
        for(unsigned i=0;i<9;++i){auto* stack=b.stack(player,i);std::uint8_t count{};if(!stack||!read(stack,b.items.itemCountOffset,count))return false;if(!count)continue;
            void* block{};void* type{};short aux{};
            if(!read(stack,0x18,block)||!read(stack,0x20,aux))return false;
            if(!block||!read(block,0x68,type)||!type)continue;
            auto name=typeName(type);if(name.empty())continue;
            // Native block-backed stacks derive aux from the Block state unless
            // the raw item aux is the wildcard sentinel.
            std::uint16_t data{};if(aux!=0x7fff&&!read(block,0x120,data))return false;
            result.copied[i]={std::move(name),count};result.types[i]=type;result.aux[i]=aux==0x7fff?0x7fff:data;
        }return true;
    }
};
}
