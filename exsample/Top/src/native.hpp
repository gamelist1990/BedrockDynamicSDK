#pragma once
// Reuse the SDK examples' guarded ECS/BlockSource reader, with a narrower
// binding set: no SpeedBuilder placement, inventory writes or Title hooks.
#include <bedrock/native_world.hpp>
#include <bedrock/native_motion.hpp>
#include "top/planner.hpp"
namespace top_native {
namespace n=bedrock::world_native;
using Vec=bedrock::Vec3;using Pos=bedrock::BlockPos;
struct Bindings : n::Bindings {
    std::size_t previousOffset{},velocityOffset{},fallOffset{},minHeightOffset{},maxHeightOffset{};
    std::uint32_t fallHash{};
    void* messageFn{};std::uintptr_t commandVtable{};std::size_t commandOffset{},guiOffset{};
    void loadTop(const std::string& profile){
        loadWorld(profile);
        const auto j=nlohmann::json::parse(profile);const auto& t=j.at("native_top");
        base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));textBegin=base+n::number(j.at("target").at("text_rva"));textEnd=textBegin+n::number(j.at("target").at("text_size"));
        std::string error;if(!bedrock::parseNativeActorLayout(profile,actors,error)||!bedrock::parseNativeItemLayout(profile,items,error))throw std::runtime_error(error);
        const auto motion=bedrock::native_motion::Layout::fromProfile(profile);
        previousOffset=motion.previous;velocityOffset=motion.velocity;fallHash=motion.fallHash;fallOffset=motion.fall;
        minHeightOffset=n::number(t.at("min_height_offset"));maxHeightOffset=n::number(t.at("max_height_offset"));
        messageFn=checked(t.at("display_message"));commandVtable=base+n::number(t.at("command_vtable_rva"));
        commandOffset=n::number(t.at("command_offset"));guiOffset=n::number(t.at("gui_offset"));checked(t.at("command_get_id"));checked(t.at("gui_getter"));
    }
    bool message(void* client,const std::string& text,const std::optional<std::string>& filtered,const std::string& tts)const noexcept{
        void* gui{};void* owner{};void* life{};unsigned char alive{};
        if(!n::read(client,guiOffset,gui)||!n::read(gui,0x30,life)||!n::read(life,0,alive)||!alive||!n::read(gui,0x98,owner)||owner!=client)return false;
        __try{reinterpret_cast<void(__fastcall*)(void*,const std::string*,const std::optional<std::string>*,const std::string*,int)>(messageFn)(gui,&text,&filtered,&tts,4);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    bool heights(void* source,int& low,int& high)const noexcept{
        short a{},z{};if(!n::read(source,minHeightOffset,a)||!n::read(source,maxHeightOffset,z)||a>=z)return false;
        low=a;high=z;return true;
    }
    bool bounds(void* player,bedrock::ActorBounds& out)const noexcept{
        auto* p=component(player,actors.aabbShapeComponentHash);
        return n::read(p,actors.aabbMinimumOffset,out.minimum)&&n::read(p,actors.aabbMaximumOffset,out.maximum)&&
            top_mod::valid({{out.minimum.x,out.minimum.y,out.minimum.z},{out.maximum.x,out.maximum.y,out.maximum.z}})&&
            out.maximum.x-out.minimum.x<=4&&out.maximum.z-out.minimum.z<=4&&out.maximum.y-out.minimum.y<=4;
    }
    // One owner-tick transaction. Preserve x/z, translate the existing AABB,
    // clear velocity and fall distance, and prevent interpolation through stone.
    bool move(void* player,float dy)const noexcept{
        if(!std::isfinite(dy))return false;
        const bedrock::native_motion::Layout motion{previousOffset,velocityOffset,fallOffset,fallHash};
        const auto get=[this](void* actor,std::uint32_t hash){return component(actor,hash);};
        bedrock::native_motion::Snapshot next;
        if(!bedrock::native_motion::capture(player,actors,motion,get,next))return false;
        const auto old=next.position;next.position.y+=dy;next.previous=next.position;next.velocity={};next.fallDistance=0;
        next.bounds=bedrock::translateBounds(next.bounds,old,next.position);
        return bedrock::native_motion::apply(player,actors,motion,get,next);
    }
};
inline bool air(std::string_view id){return id=="minecraft:air"||id=="minecraft:cave_air"||id=="minecraft:void_air";}
inline bool dangerous(std::string_view id){return !id.starts_with("minecraft:")||id.find("lava")!=id.npos||id.find("water")!=id.npos||
 id.find("fire")!=id.npos||id.find("cactus")!=id.npos||id.find("magma")!=id.npos||id.find("powder_snow")!=id.npos||
 id.find("portal")!=id.npos||id.find("berry")!=id.npos||id.find("wither_rose")!=id.npos;
}
inline bool knownFullFloor(std::string_view id){
    if(!id.starts_with("minecraft:")||dangerous(id))return false;
    const auto name=id.substr(10);
    for(const auto suffix:{"_planks","_log","_wood","_stem","_hyphae","_ore","_wool","_concrete","_terracotta","_stained_glass"})
        if(name.ends_with(suffix))return true;
    constexpr std::string_view names[]={"stone","granite","diorite","andesite","polished_granite","polished_diorite","polished_andesite",
        "dirt","grass","grass_block","coarse_dirt","rooted_dirt","podzol","mycelium",
        "cobblestone","mossy_cobblestone","smooth_stone","deepslate","cobbled_deepslate","polished_deepslate","deepslate_bricks","deepslate_tiles",
        "blackstone","polished_blackstone","polished_blackstone_bricks","obsidian","crying_obsidian","bedrock","netherrack","end_stone",
        "sandstone","smooth_sandstone","cut_sandstone","red_sandstone","smooth_red_sandstone","cut_red_sandstone","bricks","brick_block",
        "stonebrick","stone_bricks","mossy_stone_bricks","cracked_stone_bricks","chiseled_stone_bricks","nether_brick","nether_bricks","red_nether_brick","red_nether_bricks",
        "quartz_block","quartz_bricks","smooth_quartz","prismarine","prismarine_bricks","dark_prismarine","purpur_block","purpur_pillar",
        "coal_block","iron_block","gold_block","diamond_block","emerald_block","lapis_block","redstone_block","netherite_block","copper_block",
        "raw_iron_block","raw_gold_block","raw_copper_block","terracotta","hardened_clay","glass","glowstone","sea_lantern","bookshelf","hay_block"};
    return std::find(std::begin(names),std::end(names),name)!=std::end(names);
}
// Conservative safety: known full solid floor + all intersecting body cells air.
// Unsupported/custom/non-full shapes are rejected rather than guessed safe.
class Safety {
    Bindings& b;n::Reader& reader;
public:
    Safety(Bindings& bindings,n::Reader& r):b(bindings),reader(r){}
    std::string check(void* source,const bedrock::ActorBounds& bounds,int low,int high){
        const auto& a=bounds.minimum;const auto& z=bounds.maximum;
        if(a.y<low||z.y>high)return "ディメンションの高さ範囲外です";
        const int x0=static_cast<int>(std::floor(a.x+.0001f)),x1=static_cast<int>(std::floor(z.x-.0001f));
        const int z0=static_cast<int>(std::floor(a.z+.0001f)),z1=static_cast<int>(std::floor(z.z-.0001f));
        const int y0=static_cast<int>(std::floor(a.y+.0001f)),y1=static_cast<int>(std::floor(z.y-.0001f));
        for(int x=x0;x<=x1;++x)for(int k=z0;k<=z1;++k)for(int y=y0;y<=y1;++y){
            auto* state=b.block(source,{x,y,k});auto* info=reader.describe(state);
            if(!info)return "移動先のチャンクまたはブロックを読み取れません";
            if(!air(info->name))return "移動先に体・頭が埋まるブロックまたは液体があります";
        }
        if(std::abs(a.y-std::round(a.y))>.005f)return "足元の安全な支持面を確認できません";
        const int floorY=static_cast<int>(std::round(a.y))-1;
        if(floorY<low)return "足元に床がありません";
        for(int x=x0;x<=x1;++x)for(int k=z0;k<=z1;++k){
            auto* state=b.block(source,{x,floorY,k});auto* info=reader.describe(state);
            if(!info)return "足元のチャンクを読み取れません";
            if(air(info->name))return "足元に床がありません";
            if(dangerous(info->name))return "足元が危険または安全性未確認のブロックです";
            // Only solid full cubes. Bounding-shape unions of stairs/fences can
            // overstate their support; do not use them as safe full floors.
            if(!info->solid||!knownFullFloor(info->name)||info->name.find("stairs")!=info->name.npos||info->name.find("slab")!=info->name.npos||
               info->name.find("fence")!=info->name.npos||info->name.find("wall")!=info->name.npos||info->name.find("trapdoor")!=info->name.npos)
                return "足元が安全な全面支持ブロックではありません";
        }
        return {};
    }
};
}
