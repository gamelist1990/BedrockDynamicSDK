#pragma once
// Reuse the SDK examples' guarded ECS/BlockSource reader, with a narrower
// binding set: no SpeedBuilder placement, inventory writes or Title hooks.
#include "../../SpeedBuilder/src/native.hpp"
#include "top/planner.hpp"
namespace top_native {
namespace n=speedbuilder_native;
using Vec=bedrock::Vec3;using Pos=speed_builder::BlockPos;
struct Bindings : n::Bindings {
    std::size_t previousOffset{},velocityOffset{},fallOffset{},minHeightOffset{},maxHeightOffset{};
    std::uint32_t fallHash{};
    void* messageFn{};std::uintptr_t commandVtable{};std::size_t commandOffset{},guiOffset{};
    void loadTop(const std::string& profile){
        const auto j=nlohmann::json::parse(profile);const auto& t=j.at("native_top");
        base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));textBegin=base+n::number(j.at("target").at("text_rva"));textEnd=textBegin+n::number(j.at("target").at("text_size"));
        std::string error;if(!bedrock::parseNativeActorLayout(profile,actors,error)||!bedrock::parseNativeItemLayout(profile,items,error))throw std::runtime_error(error);
        const auto& w=j.at("native_speed_builder");regionFn=checked(w.at("get_region"));getBlockFn=checked(w.at("get_block"));
        const auto& f=j.at("native_freecam").at("borion");
        previousOffset=n::number(f.at("state_previous_position_offset"));velocityOffset=n::number(f.at("state_velocity_offset"));fallHash=n::number(f.at("fall_distance_component_hash"));fallOffset=n::number(f.at("fall_distance_value_offset"));
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
        auto* state=component(player,actors.stateVectorComponentHash);auto* shape=component(player,actors.aabbShapeComponentHash);auto* fall=component(player,fallHash);
        Vec pos{},prev{},vel{},a{},z{};float distance{};
        if(!std::isfinite(dy)||!n::read(state,actors.statePositionOffset,pos)||!n::read(state,previousOffset,prev)||!n::read(state,velocityOffset,vel)||
           !n::read(shape,actors.aabbMinimumOffset,a)||!n::read(shape,actors.aabbMaximumOffset,z)||!n::read(fall,fallOffset,distance))return false;
        Vec next=pos;next.y+=dy;Vec nextA=a,nextZ=z;nextA.y+=dy;nextZ.y+=dy;
        const bool ok=n::write(state,actors.statePositionOffset,next)&&n::write(state,previousOffset,next)&&n::write(state,velocityOffset,Vec{})&&
            n::write(shape,actors.aabbMinimumOffset,nextA)&&n::write(shape,actors.aabbMaximumOffset,nextZ)&&n::write(fall,fallOffset,0.f);
        if(!ok){n::write(state,actors.statePositionOffset,pos);n::write(state,previousOffset,prev);n::write(state,velocityOffset,vel);
            n::write(shape,actors.aabbMinimumOffset,a);n::write(shape,actors.aabbMaximumOffset,z);n::write(fall,fallOffset,distance);}
        return ok;
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
