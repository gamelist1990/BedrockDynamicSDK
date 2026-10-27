#pragma once
#include <bedrock/blocks.hpp>
#include <nlohmann/json.hpp>
#include <cmath>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace block_outline {
struct Target { std::string id, name; std::uint32_t color{}; };
struct Config {
    bool enabled=true, showNames=true, debug=false;
    float range=16.f, nameScale=.25f;
    unsigned reads=128, budgetUs=400, maximum=256, labels=64;
    std::uint64_t revision{};
    std::vector<Target> targets;
    std::unordered_map<std::string,unsigned> lookup;
};
inline std::uint32_t color(std::string value) {
    if(value.size()!=7 || value[0]!='#' || value.find_first_not_of("0123456789abcdefABCDEF",1)!=value.npos)
        throw std::invalid_argument("color must be #RRGGBB");
    return static_cast<std::uint32_t>(std::stoul(value.substr(1),nullptr,16));
}
inline nlohmann::json defaults() {
    return {{"enabled",true},{"range",16},{"show_names",true},{"name_scale",.25},{"color","#4CE684"},
        {"scan_blocks_per_tick",128},{"scan_budget_us",400},{"max_results",256},{"max_labels",64},{"debug",false},
        {"blocks",nlohmann::json::array({{{"id","minecraft:diamond_ore"},{"name","ダイヤモンド鉱石"},{"color","#45DDEE"}},
            {{"id","minecraft:deepslate_diamond_ore"},{"name","深層ダイヤモンド鉱石"},{"color","#45DDEE"}}})}};
}
inline Config parse(const nlohmann::json& value) {
    Config c;
    c.enabled=value.value("enabled",true); c.showNames=value.value("show_names",true); c.debug=value.value("debug",false);
    c.range=value.value("range",16.f); c.nameScale=value.value("name_scale",.25f);
    const auto bounded=[&](const char* key,int fallback,int low,int high) {
        const auto n=value.value(key,static_cast<long long>(fallback));
        if(n<low || n>high)throw std::invalid_argument(std::string(key)+" out of range");
        return static_cast<unsigned>(n);
    };
    c.reads=bounded("scan_blocks_per_tick",128,1,512); c.budgetUs=bounded("scan_budget_us",400,100,2000);
    c.maximum=bounded("max_results",256,1,1024); c.labels=bounded("max_labels",64,0,128);
    if(!std::isfinite(c.range)||c.range<1||c.range>128||!std::isfinite(c.nameScale)||c.nameScale<.1f||c.nameScale>1.f)
        throw std::invalid_argument("range must be 1..128, name_scale .1..1");
    const auto common=color(value.value("color",std::string{"#4CE684"}));
    const auto& blocks=value.at("blocks");
    if(!blocks.is_array()||blocks.size()>64)throw std::invalid_argument("blocks must contain at most 64 entries");
    for(const auto& block:blocks) {
        auto id=block.is_string()?block.get<std::string>():block.at("id").get<std::string>();
        bedrock::BlockIdentifier valid{id};
        if(id.size()>128||id=="minecraft:air"||id=="minecraft:cave_air"||id=="minecraft:void_air")
            throw std::invalid_argument("block ID is too long or is air");
        auto name=block.is_string()?id:block.value("name",id);
        if(name.empty()||name.size()>192||name.find_first_of("\r\n\t")!=name.npos||name.find('\0')!=name.npos)
            throw std::invalid_argument("block name must be one nonempty UTF-8 line, at most 192 bytes");
        const auto tint=block.is_string()?common:color(block.value("color",value.value("color",std::string{"#4CE684"})));
        if(!c.lookup.emplace(id,static_cast<unsigned>(c.targets.size())).second)throw std::invalid_argument("duplicate block ID");
        c.targets.push_back({std::move(id),std::move(name),tint});
    }
    return c;
}
// Near-first 16^3 sections; only intersecting sections are queued. Each cell
// belongs to exactly one section, including negative world coordinates.
class ScanCursor {
public:
    void reset(bedrock::BlockPos anchor,float range) {
        sections_.clear(); section_=0; cell_=0;
        const auto div=[](int n){return int((std::int64_t(n)-(n<0?15:0))/16);};
        const int r=int(std::ceil(range))+1;
        for(int y=div(anchor.y-r);y<=div(anchor.y+r);++y)
            for(int z=div(anchor.z-r);z<=div(anchor.z+r);++z)
                for(int x=div(anchor.x-r);x<=div(anchor.x+r);++x) {
                    bedrock::BlockPos p{x*16,y*16,z*16};
                    const auto axis=[](int a,int lo){return std::max({float(lo)-float(a),0.f,float(a)-float(lo+16)});};
                    const float dx=axis(anchor.x,p.x),dy=axis(anchor.y,p.y),dz=axis(anchor.z,p.z);
                    if(dx*dx+dy*dy+dz*dz<=(range+2)*(range+2))sections_.push_back(p);
                }
        const auto distance=[&](bedrock::BlockPos p){
            const double x=double(p.x)+8-anchor.x,y=double(p.y)+8-anchor.y,z=double(p.z)+8-anchor.z;
            return x*x+y*y+z*z;
        };
        std::sort(sections_.begin(),sections_.end(),[&](auto a,auto b){return distance(a)<distance(b);});
    }
    void skipSection() noexcept { if(cell_){cell_=0;++section_;} }
    void rewind() noexcept {section_=0;cell_=0;}
    std::optional<bedrock::BlockPos> next() noexcept {
        if(section_>=sections_.size())return std::nullopt;
        const auto p=sections_[section_];const unsigned n=cell_++;
        if(cell_==4096){cell_=0;++section_;}
        return bedrock::BlockPos{p.x+int(n&15),p.y+int(n>>8),p.z+int((n>>4)&15)};
    }
private:
    std::vector<bedrock::BlockPos> sections_;
    std::size_t section_{};unsigned cell_{};
};
} // namespace block_outline
