#pragma once
#include <bedrock/profiles/profile_json.hpp>
// Windows x64 native adapter. Call only on the owning game tick after exact
// Resolver identity validation. Prefer NativeWorld tick-scoped public API.
#if !defined(_WIN32) || !defined(_MSC_VER) || !defined(_WIN64)
#error "Native world access requires Windows x64 MSVC"
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bedrock/api/geometry.hpp>
#include <bedrock/native/actors.hpp>
#include <bedrock/native/items.hpp>
#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <bedrock/api/blocks.hpp>
#include <array>
#include <algorithm>
#include <cmath>
#include <vector>
#include <stdexcept>
#include <unordered_map>
#include <cstring>
#include <cstddef>

namespace bedrock::world_native {
struct Entity{std::uint32_t value;constexpr operator std::uint32_t()const{return value;}constexpr bool operator==(const Entity&)const=default;};
struct Traits{using value_type=Entity;using entity_type=std::uint32_t;using version_type=std::uint16_t;
    static constexpr entity_type entity_mask=0x3ffff;static constexpr version_type version_mask=0x3fff;};
}
namespace entt{template<>struct entt_traits<bedrock::world_native::Entity>:basic_entt_traits<bedrock::world_native::Traits>{static constexpr std::size_t page_size=2048;};}
namespace bedrock::world_native {
using Pos=bedrock::BlockPos;using Vec=bedrock::Vec3;
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
    bool placementReady{},scanReady{};
    struct ScanLayout { std::size_t minY{},maxY{},begin{},end{},stride{},storage{},getSlot{}; } scan;
    struct ScanSection { void* storage{}; bool empty{},unavailable{}; };
    void* regionFn{};void* getBlockFn{};void* selectFn{};void* getHitFn{};

    std::array<std::uintptr_t,2> gameTables{};
    std::vector<std::uintptr_t> sourceVtables;
    void* getChunkFn{};
    struct LayoutHash {
        using is_transparent=void;
        std::size_t operator()(std::string_view name)const noexcept{return std::hash<std::string_view>{}(name);}
    };
    std::unordered_map<std::string,std::size_t,LayoutHash,std::equal_to<>> offsets;
    std::size_t layout(std::string_view name)const{
        const auto it=offsets.find(name);if(it==offsets.end())throw std::runtime_error("missing native world layout: "+std::string(name));return it->second;
    }

    bool executable(void* p)const noexcept{const auto n=reinterpret_cast<std::uintptr_t>(p);return n>=textBegin&&n<textEnd;}
    void* checked(const nlohmann::json& fn)const{
        auto* p=reinterpret_cast<void*>(base+number(fn.at("rva")));if(!executable(p))throw std::runtime_error("native target outside .text");
        const auto guard=fn.at("entry_check").get<std::string>();if(guard.size()<23||guard.size()>191||(guard.size()+1)%3)throw std::runtime_error("bad native guard");
        for(std::size_t i=0;i<guard.size();i+=3){unsigned char value{};if(!read(p,i/3,value)||value!=std::stoul(guard.substr(i,2),nullptr,16))throw std::runtime_error("native entry mismatch");}return p;
    }
    void loadWorld(const std::string& profile){const auto j=bedrock::parseNativeProfile(profile);const auto& f=j.at("native_world");
        if(f.at("schema")!=1||f.value("revision",0)<3)throw std::runtime_error("unsupported native world schema");
        base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));textBegin=base+number(j.at("target").at("text_rva"));textEnd=textBegin+number(j.at("target").at("text_size"));
        std::string error;if(!bedrock::parseNativeActorLayout(profile,actors,error)||!bedrock::parseNativeItemLayout(profile,items,error))throw std::runtime_error(error);
        offsets.clear();
        for(const auto& [key,value]:f.at("candidate_layouts").items())
            if(value.is_number_unsigned()||value.is_number_integer()||(value.is_string()&&value.get_ref<const std::string&>().starts_with("0x"))){
                const auto n=number(value);if(n>65536)throw std::runtime_error("native world layout out of range");offsets.emplace(key,n);
            }
        (void)layout("block_serialization_tag_offset");
        (void)layout("block_source_get_block_slot");
        (void)layout("block_source_get_chunk_slot");
        (void)layout("block_state_data_offset");
        (void)layout("block_type_collision_shape_slot");
        (void)layout("block_type_flags_offset");
        (void)layout("block_type_full_name_string_offset");
        (void)layout("block_type_offset");
        (void)layout("block_type_placement_state_slot");
        (void)layout("block_type_solid_mask");
        (void)layout("compound_tag_head_offset");
        (void)layout("compound_tag_size_offset");
        (void)layout("game_mode_build_block_slot");
        (void)layout("game_mode_drag_active_offset");
        (void)layout("game_mode_drag_previous_offset");
        (void)layout("item_stack_aux_offset");
        (void)layout("item_stack_block_offset");
        (void)layout("level_hit_result_slot");
        (void)layout("player_game_mode_offset");
        (void)layout("player_level_offset");
        (void)layout("tag_get_id_slot");
        (void)layout("tag_scalar_or_string_offset");
        (void)layout("tag_variant_index_offset");
        (void)layout("tree_key_offset");
        (void)layout("tree_left_offset");
        (void)layout("tree_nil_offset");
        (void)layout("tree_parent_offset");
        (void)layout("tree_right_offset");
        (void)layout("tree_tag_variant_offset");
        sourceVtables.clear();
        if(f.contains("block_source_vtable_candidates")) {
            const auto& tables=f.at("block_source_vtable_candidates");
            if(!tables.is_array()||tables.empty()||tables.size()>8)throw std::runtime_error("invalid BlockSource vtable candidates");
            for(const auto& value:tables){
                const auto rva=number(value);
                if(!rva||rva>=number(j.at("target").at("size_of_image"))||
                   std::find(sourceVtables.begin(),sourceVtables.end(),base+rva)!=sourceVtables.end())
                    throw std::runtime_error("invalid or duplicate BlockSource vtable candidate");
                sourceVtables.push_back(base+rva);
            }
        }
        regionFn=checked(f.at("get_region"));getBlockFn=checked(f.at("get_block"));
        getChunkFn=f.contains("get_chunk")?checked(f.at("get_chunk")):nullptr;
        scanReady=false;
        if(f.contains("subchunk_scan")) {
            const auto& j=f.at("subchunk_scan");checked(j.at("guard"));
            scan={number(j.at("source_min_y")),number(j.at("source_max_y")),number(j.at("chunk_sections_begin")),
                number(j.at("chunk_sections_end")),number(j.at("section_stride")),number(j.at("section_storage")),number(j.at("storage_get_slot"))};
            if(scan.minY>65536||scan.maxY>65536||scan.begin>65536||scan.end!=scan.begin+8||
               scan.stride<8||scan.stride>4096||scan.storage+8>scan.stride||scan.getSlot>256)
                throw std::runtime_error("invalid SubChunk scan layout");
            scanReady=j.value("enabled",true);
        }
        placementReady=false;
        try{selectFn=checked(f.at("select_hotbar"));getHitFn=checked(f.at("get_hit_result"));
        checked(f.at("build_block"));checked(f.at("base_placement_state"));
        for(unsigned i=0;i<2;++i)gameTables[i]=base+number(f.at("game_mode_vtables").at(i));
        placementReady=true;}catch(const std::exception&){selectFn=getHitFn=nullptr;}
        if(layout("hit_result_scalar_prefix_size")!=sizeof(HitHead)||layout("hit_type_offset")!=offsetof(HitHead,type)||
           layout("hit_face_offset")!=offsetof(HitHead,face)||layout("hit_block_offset")!=offsetof(HitHead,block)||
           layout("hit_position_offset")!=offsetof(HitHead,hit))placementReady=false;
    }
    void load(const std::string& profile){
        loadWorld(profile);
        if(!placementReady)throw std::runtime_error("native block placement unavailable");
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
    bool candidateBlockSource(void* source)const noexcept{
        if(sourceVtables.empty())return source!=nullptr;
        std::uintptr_t table{};
        if(!read(source,0,table))return false;
        return std::find(sourceVtables.begin(),sourceVtables.end(),table)!=sourceVtables.end();
    }
    bool pose(void* player,Vec& position,std::uint64_t& runtime)const noexcept{
        return read(component(player,actors.stateVectorComponentHash),actors.statePositionOffset,position)&&
            read(component(player,actors.runtimeIdComponentHash),actors.runtimeIdValueOffset,runtime)&&runtime&&
            std::isfinite(position.x)&&std::isfinite(position.y)&&std::isfinite(position.z)&&
            std::abs(position.x)<30000000&&std::abs(position.z)<30000000&&std::abs(position.y)<10000;
    }
    void* gameMode(void* player)const noexcept{
        void* gm{};std::uintptr_t vt{};void* owner{};
        if(!read(player,layout("player_game_mode_offset"),gm)||!read(gm,0,vt)||(vt!=gameTables[0]&&vt!=gameTables[1])||!read(gm,8,owner)||owner!=player)return nullptr;return gm;
    }
    void* loadedChunk(void* source,int x,int z)const noexcept {
        void** vt{};void* fn{};
        if(!candidateBlockSource(source)||!read(source,0,vt)||
           !read(vt,layout("block_source_get_chunk_slot")*8,fn)||
           (getChunkFn?fn!=getChunkFn:!executable(fn)))return nullptr;
        const std::array<int,2> chunk{x,z};
        __try{return reinterpret_cast<void*(__fastcall*)(void*,const void*)>(fn)(source,&chunk);}__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
    }
    ScanSection section(void* source,void* chunk,int y)const noexcept {
        std::int16_t low{},high{};
        if(!scanReady||!read(source,scan.minY,low)||!read(source,scan.maxY,high)||low>=high||(low&15))return {nullptr,false,true};
        if(y<low||y>=high)return {nullptr,true,false};
        if(!chunk)return {nullptr,false,true};
        std::uintptr_t begin{},end{};
        if(!read(chunk,scan.begin,begin)||!read(chunk,scan.end,end)||!begin||end<begin||
           (end-begin)%scan.stride||(end-begin)/scan.stride>4096)return {nullptr,false,true};
        const auto index=static_cast<unsigned>(y-low)/16;
        if(index>=(end-begin)/scan.stride)return {nullptr,true,false};
        void* storage{};
        if(!read(reinterpret_cast<void*>(begin),index*scan.stride+scan.storage,storage))return {nullptr,false,true};
        return {storage,!storage,false};
    }
    void* sectionBlock(void* storage,unsigned index)const noexcept {
        void** vt{};void* fn{};
        if(index>=4096||!read(storage,0,vt)||!read(vt,scan.getSlot*8,fn)||!executable(fn))return nullptr;
        __try{return reinterpret_cast<void*(__fastcall*)(void*,unsigned)>(fn)(storage,index);}__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
    }
    void* block(void* source,const Pos& pos,bool loaded=false)const noexcept{
        void** vt{};void* fn{};void* chunkFn{};
        if(!candidateBlockSource(source)||!read(source,0,vt)||
           !read(vt,layout("block_source_get_block_slot")*8,fn)||fn!=getBlockFn||
           !read(vt,layout("block_source_get_chunk_slot")*8,chunkFn)||
           (getChunkFn?chunkFn!=getChunkFn:!executable(chunkFn)))return nullptr;
        const std::array<int,2> chunk{pos.x>>4,pos.z>>4};
        __try{if(!loaded&&!reinterpret_cast<void*(__fastcall*)(void*,const void*)>(chunkFn)(source,&chunk))return nullptr;
            return reinterpret_cast<void*(__fastcall*)(void*,const Pos*)>(fn)(source,&pos);}__except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
    }
    // Same hidden-return ABI used by InsideCubeRenderer. Query only support
    // candidates, not the full preview volume. Non-solid slabs/chests can still
    // provide a usable collision surface.
    bool collision(void* type,void* state,void* source,const Pos& pos)const noexcept{
        void** vt{};void* fn{};
        if(!read(type,0,vt)||!read(vt,layout("block_type_collision_shape_slot")*8,fn)||!executable(fn))return false;
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
        if(!read(rot,actors.rotationYawOffset,oldYaw)||!read(rot,actors.rotationPitchOffset,oldPitch)||!read(type,0,vt)||!read(vt,layout("block_type_placement_state_slot")*8,fn)||!executable(fn))return false;
        bool ok=false;
        __try{__try{if(write(rot,actors.rotationYawOffset,yaw)&&write(rot,actors.rotationPitchOffset,pitch)){
            *result=reinterpret_cast<void*(__fastcall*)(void*,void*,const Pos*,unsigned char,const Vec*,int)>(fn)(type,player,&target,static_cast<unsigned char>(face),&fractional,aux);ok=*result!=nullptr;
        }}__finally{write(rot,actors.rotationYawOffset,oldYaw);write(rot,actors.rotationPitchOffset,oldPitch);}}
        __except(EXCEPTION_EXECUTE_HANDLER){ok=false;}return ok;
    }
    struct HitHead{Vec start,direction;int type;unsigned char face,padding[3];Pos block;Vec hit;};
    static_assert(sizeof(HitHead)==0x38);
    bool build(void* player,void* gm,const Pos& support,unsigned face,Vec eye,Vec hit,bool* accepted)const noexcept{
        if(!placementReady||!accepted||face>5)return false;
        void* level{};void** vt{};void* getter{};void* head{};void** gameVt{};void* buildFn{};
        if(!read(player,layout("player_level_offset"),level)||!read(level,0,vt)||!read(vt,layout("level_hit_result_slot")*8,getter)||getter!=getHitFn||
           !read(gm,0,gameVt)||!read(gameVt,layout("game_mode_build_block_slot")*8,buildFn)||!executable(buildFn))return false;
        HitHead original{};bool saved=false,called=false,oldDrag{},oldPrevious{};
        if(!read(gm,layout("game_mode_drag_active_offset"),oldDrag)||!read(gm,layout("game_mode_drag_previous_offset"),oldPrevious))return false;
        __try{__try{
            head=reinterpret_cast<void*(__fastcall*)(void*)>(getter)(level);if(!read(head,0,original))return false;saved=true;
            auto input=original;input.start=eye;input.direction={hit.x-eye.x,hit.y-eye.y,hit.z-eye.z};input.type=0;input.face=static_cast<unsigned char>(face);input.block=support;input.hit=hit;
            if(!write(head,0,input))return false;
            // New placement, not vanilla's drag-direction continuation.
            const bool no=false;if(!write(gm,layout("game_mode_drag_active_offset"),no)||!write(gm,layout("game_mode_drag_previous_offset"),no))return false;
            *accepted=reinterpret_cast<bool(__fastcall*)(void*,const Pos*,unsigned char,unsigned char,bool)>(buildFn)(gm,&support,static_cast<unsigned char>(face),0,false);called=true;
        }__finally{if(saved)write(head,0,original);write(gm,layout("game_mode_drag_active_offset"),oldDrag);write(gm,layout("game_mode_drag_previous_offset"),oldPrevious);}}
        __except(EXCEPTION_EXECUTE_HANDLER){called=false;}return called;
    }
};
inline bool identifier(std::string_view name){if(name.empty()||name.size()>256||name.find(':')==name.npos)return false;
    return std::all_of(name.begin(),name.end(),[](unsigned char c){return c>32&&c<127;});}
// Bounded read-only view of the exact-build MSVC map/TagVariant layout.
// No foreign STL methods or native NBT constructors are called.
inline bool tagEntries(const Bindings& b,const void* compound,std::vector<std::pair<std::string,void*>>& entries){
    void* head{};std::uint64_t count{};unsigned char nil{};void* root{};
    if(!read(compound,b.layout("compound_tag_head_offset"),head)||!read(compound,b.layout("compound_tag_size_offset"),count)||count>128||!read(head,b.layout("tree_nil_offset"),nil)||nil!=1||!read(head,b.layout("tree_parent_offset"),root))return false;
    if(!count)return root==head;
    void* parent{};if(!root||root==head||!read(root,b.layout("tree_parent_offset"),parent)||parent!=head)return false;
    std::array<void*,128> queue{};queue[0]=root;std::size_t used=1;
    for(std::size_t i=0;i<used;++i){auto* node=queue[i];unsigned char type{};StringCopy key;
        if(!read(node,b.layout("tree_nil_offset"),nil)||nil||!copyString(static_cast<const std::byte*>(node)+b.layout("tree_key_offset"),&key)||key.size>128||!key.size||!read(node,b.layout("tree_tag_variant_offset")+b.layout("tag_variant_index_offset"),type)||type>11)return false;
        std::string name(key.bytes,key.size);if(!std::all_of(name.begin(),name.end(),[](unsigned char c){return c>32&&c<127;}))return false;
        entries.emplace_back(std::move(name),static_cast<std::byte*>(node)+b.layout("tree_tag_variant_offset"));
        for(auto offset:{b.layout("tree_left_offset"),b.layout("tree_right_offset")}){void* child{};if(!read(node,offset,child)||!read(child,b.layout("tree_nil_offset"),nil))return false;if(nil)continue;
            if(used>=count||used>=queue.size()||!read(child,b.layout("tree_parent_offset"),parent)||parent!=node||std::find(queue.begin(),queue.begin()+used,child)!=queue.begin()+used)return false;
            queue[used++]=child;
        }
    }return used==count;
}
inline std::shared_ptr<const bedrock::BlockProperties> properties(const Bindings& b,void* state,std::string_view expectedName){
    const auto* root=static_cast<const std::byte*>(state)+b.layout("block_serialization_tag_offset");void** vt{};void* idFn{};std::array<unsigned char,6> code{};
    if(!read(root,0,vt)||!read(vt,b.layout("tag_get_id_slot")*8,idFn)||!b.executable(idFn)||!read(idFn,0,code))return {};
    const bool compound=(code[0]==0xb0&&code[1]==10&&code[2]==0xc3)||(code[0]==0xb8&&code[1]==10&&code[2]==0&&code[3]==0&&code[4]==0&&code[5]==0xc3);
    if(!compound)return {};
    std::vector<std::pair<std::string,void*>> entries;if(!tagEntries(b,root,entries))return {};
    void* states{};bool nameMatched=false;
    for(const auto& [key,value]:entries){unsigned char type{};if(!read(value,b.layout("tag_variant_index_offset"),type))return {};
        if(key=="name"&&type==8){StringCopy name;if(!copyString(static_cast<const std::byte*>(value)+b.layout("tag_scalar_or_string_offset"),&name))return {};nameMatched=std::string_view(name.bytes,name.size)==expectedName;}
        if(key=="states"&&type==10)states=value;
    }
    if(!states||!nameMatched)return {};
    entries.clear();if(!tagEntries(b,states,entries))return {};
    auto result=std::make_shared<bedrock::BlockProperties>();
    for(const auto& [key,value]:entries){unsigned char type{};if(!read(value,b.layout("tag_variant_index_offset"),type))return {};
        if(type==1){signed char n{};if(!read(value,b.layout("tag_scalar_or_string_offset"),n))return {};if(!result->values.emplace(key,int(n)).second)return {};}
        else if(type==3){int n{};if(!read(value,b.layout("tag_scalar_or_string_offset"),n))return {};if(!result->values.emplace(key,n).second)return {};}
        else if(type==8){StringCopy text;if(!copyString(static_cast<const std::byte*>(value)+b.layout("tag_scalar_or_string_offset"),&text)||text.size>256)return {};if(!result->values.emplace(key,std::string(text.bytes,text.size)).second)return {};}
        else return {};
    }return result;
}
struct BlockInfo{std::string name;void* type{};bool solid{};std::shared_ptr<const bedrock::BlockProperties> properties;};
struct NativeHotbar{bedrock::Hotbar copied{};std::array<void*,9> types{};std::array<int,9> aux{};};
class Reader{
public:
    Bindings& b;std::unordered_map<void*,BlockInfo> states;std::unordered_map<void*,std::string> names;
    explicit Reader(Bindings& bindings):b(bindings){}
    void clear(){states.clear();names.clear();}
    std::string typeName(void* type){if(auto it=names.find(type);it!=names.end())return it->second;
        StringCopy value;if(!copyString(static_cast<std::byte*>(type)+b.layout("block_type_full_name_string_offset"),&value))return {};
        std::string name(value.bytes,value.size);if(!identifier(name))return {};if(names.size()<4096)names.emplace(type,name);return name;}
    BlockInfo* describe(void* state){
        if(!state)return nullptr;auto it=states.find(state);if(it!=states.end())return &it->second;if(states.size()>=8192)return nullptr;
        BlockInfo info;std::uint8_t flags{};if(!read(state,b.layout("block_type_offset"),info.type)||!info.type)return nullptr;
        info.name=typeName(info.type);if(info.name.empty()||!read(info.type,b.layout("block_type_flags_offset"),flags))return nullptr;
        info.solid=(flags&b.layout("block_type_solid_mask"))!=0;info.properties=properties(b,state,info.name);
        return &states.emplace(state,std::move(info)).first->second;
    }
    bedrock::Cell describeCell(void* state,Pos pos={}){
        bedrock::Cell result;result.position=pos;auto* info=describe(state);if(!info)return result;
        result.identifier=info->name;result.stateIdentity=reinterpret_cast<std::uintptr_t>(state);result.properties=info->properties;
        std::uint16_t data{};if(read(state,b.layout("block_state_data_offset"),data))result.legacyData=data;
        if(info->name=="minecraft:air"||info->name=="minecraft:cave_air"||info->name=="minecraft:void_air")result.kind=bedrock::CellKind::air;
        else{result.kind=bedrock::CellKind::block;result.supportsPlacement=info->solid;result.nativePlacementCandidate=true;
            bedrock::PlacementRecipe recipe;recipe.itemIdentifier=result.identifier;result.placement=recipe;}
        return result;
    }
    bedrock::Cell getBlock(void* source,Pos pos,bool inspectSupport=false){
        auto* state=b.block(source,pos);auto result=describeCell(state,pos);
        if(inspectSupport&&result.kind==bedrock::CellKind::block&&!result.supportsPlacement){auto* info=describe(state);result.supportsPlacement=info&&b.collision(info->type,state,source,pos);}
        return result;
    }
    bedrock::Cell cell(void* source,Pos pos,bool inspectSupport=false){return getBlock(source,pos,inspectSupport);}
    bool hotbar(void* player,NativeHotbar& result){result={};
        for(unsigned i=0;i<9;++i){auto* stack=b.stack(player,i);std::uint8_t count{};if(!stack||!read(stack,b.items.itemCountOffset,count))return false;if(!count)continue;
            void* block{};void* type{};short aux{};
            if(!read(stack,b.layout("item_stack_block_offset"),block)||!read(stack,b.layout("item_stack_aux_offset"),aux))return false;
            if(!block||!read(block,b.layout("block_type_offset"),type)||!type)continue;
            auto name=typeName(type);if(name.empty())continue;
            // Native block-backed stacks derive aux from the Block state unless
            // the raw item aux is the wildcard sentinel.
            std::uint16_t data{};if(aux!=0x7fff&&!read(block,b.layout("block_state_data_offset"),data))return false;
            result.copied[i]={std::move(name),count};result.types[i]=type;result.aux[i]=aux==0x7fff?0x7fff:data;
        }return true;
    }
};
}
