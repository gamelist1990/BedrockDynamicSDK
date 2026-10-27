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
    c.reads=bounded("scan_blocks_per_tick",128,1,4096); c.budgetUs=bounded("scan_budget_us",400,100,5000);
    c.maximum=bounded("max_results",256,1,1024); c.labels=bounded("max_labels",64,0,128);
    if(!std::isfinite(c.range)||c.range<1||c.range>500||!std::isfinite(c.nameScale)||c.nameScale<.1f||c.nameScale>1.f)
        throw std::invalid_argument("range must be 1..500, name_scale .1..1");
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
// Constant-memory, expanding section shells. Reset is O(1), even at radius
// 500. At most 32 section candidates are examined per call; callers may yield
// on nullopt unless finished() is true. Cell order matches native Y-Z-X indices.
class ScanCursor {
public:
    void reset(bedrock::BlockPos anchor,float range) noexcept {
        anchor_=anchor;range_=range;
        const auto div=[](int n){return int((std::int64_t(n)-(n<0?15:0))/16);};
        origin_={div(anchor.x),div(anchor.y),div(anchor.z)};
        limit_=int(std::ceil(range/16))+1;
        rewind();
    }
    void skipSection() noexcept { cell_=4096; }
    void rewind() noexcept { radius_=face_=index_=0;cell_=4096;finished_=false; }
    bool finished() const noexcept { return finished_; }
    std::optional<bedrock::BlockPos> next() noexcept {
        if(finished_)return std::nullopt;
        if(cell_==4096) {
            bool selected=false;
            for(unsigned attempts=0;attempts<32;++attempts) {
                const auto offset=nextSection();
                if(!offset){finished_=true;return std::nullopt;}
                const bedrock::BlockPos p{(origin_.x+offset->x)*16,(origin_.y+offset->y)*16,(origin_.z+offset->z)*16};
                const auto axis=[](int a,int lo){return std::max({float(lo)-float(a),0.f,float(a)-float(lo+16)});};
                const float dx=axis(anchor_.x,p.x),dy=axis(anchor_.y,p.y),dz=axis(anchor_.z,p.z);
                if(dx*dx+dy*dy+dz*dz>(range_+2)*(range_+2))continue;
                section_=p;cell_=0;selected=true;break;
            }
            if(!selected)return std::nullopt;
        }
        const unsigned n=cell_++;
        return bedrock::BlockPos{section_.x+int(n>>8),section_.y+int(n&15),section_.z+int((n>>4)&15)};
    }
private:
    std::optional<bedrock::BlockPos> nextSection() noexcept {
        if(!radius_){radius_=1;return bedrock::BlockPos{};}
        while(radius_<=limit_) {
            if(face_==6){++radius_;face_=index_=0;continue;}
            const int side=radius_*2+1,inner=side-2;
            const int width=face_<2?side:inner,height=face_<4?side:inner;
            if(index_>=width*height){++face_;index_=0;continue;}
            const int a=index_%width,b=index_/width;++index_;
            if(face_<2)return bedrock::BlockPos{face_?-radius_:radius_,a-radius_,b-radius_};
            if(face_<4)return bedrock::BlockPos{a-radius_+1,face_==2?-radius_:radius_,b-radius_};
            return bedrock::BlockPos{a-radius_+1,b-radius_+1,face_==4?-radius_:radius_};
        }
        return std::nullopt;
    }
    bedrock::BlockPos anchor_{},origin_{},section_{};
    float range_{};
    int limit_{},radius_{},face_{},index_{};
    unsigned cell_=4096;
    bool finished_{};
};
} // namespace block_outline
