#pragma once
#include "box_cache.hpp"
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace hitbox {
// Copied identity only. Workers never dereference an Actor handle.
struct NameKey {
    std::uint64_t epoch{};
    std::uintptr_t actor{};
    std::uint32_t entity{}; // Includes EnTT generation, preventing pointer-reuse matches.
    bool operator==(const NameKey&) const = default;
};
inline std::string normalizedName(std::string_view input) {
    std::string out;
    bool formatting=false;
    std::size_t characters=0;
    for(std::size_t i=0;i<input.size() && characters<96;) {
        const auto start=i;
        const auto first=static_cast<unsigned char>(input[i++]);
        std::uint32_t cp=first;
        unsigned rest=0;
        if(first>=0xc2 && first<=0xdf) { cp=first&31; rest=1; }
        else if(first>=0xe0 && first<=0xef) { cp=first&15; rest=2; }
        else if(first>=0xf0 && first<=0xf4) { cp=first&7; rest=3; }
        else if(first>=0x80) continue;
        if(i+rest>input.size()) break;
        bool valid=true;
        for(unsigned j=0;j<rest;++j) {
            const auto byte=static_cast<unsigned char>(input[i+j]);
            if((byte&0xc0)!=0x80) { valid=false; break; }
            cp=(cp<<6)|(byte&63);
        }
        if(!valid) continue;
        i+=rest;
        if((rest==1 && cp<0x80) || (rest==2 && cp<0x800) ||
           (rest==3 && cp<0x10000) || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) continue;
        if(cp=='\n' || cp=='\r' || cp==0x2028 || cp==0x2029) break;
        if(formatting) { formatting=false; continue; }
        if(cp==0xa7) { formatting=true; continue; }
        if(cp<32 || (cp>=127 && cp<=159)) continue;
        out.append(input.substr(start,i-start)); ++characters;
    }
    const auto begin=out.find_first_not_of(' ');
    if(begin==std::string::npos) return {};
    return out.substr(begin,out.find_last_not_of(' ')-begin+1);
}

// Bounded four-way mailbox. Native hook callbacks only try-lock and copy bytes;
// no allocation, native component lookup, font work, or blocking on a worker.
class NameMailbox {
public:
    static constexpr std::size_t maximumBytes=384;
    struct Value { std::array<char,maximumBytes> bytes{}; std::size_t size{}; std::uint64_t revision{}; };
    bool observe(NameKey key,std::string_view text) noexcept {
        if(!key.epoch || !key.actor || text.empty()) return false;
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock) return false;
        auto* slot=&slots_[bucket(key)];
        auto* selected=slot;
        for(std::size_t i=0;i<ways;++i) {
            if(slot[i].value.revision && slot[i].key==key) { selected=&slot[i]; break; }
            if(!slot[i].value.revision || slot[i].value.revision<selected->value.revision) selected=&slot[i];
        }
        const auto length=std::min(text.size(),maximumBytes);
        if(selected->value.revision && selected->key==key && selected->value.size==length &&
           !std::memcmp(selected->value.bytes.data(),text.data(),length)) return false;
        selected->key=key;
        std::memcpy(selected->value.bytes.data(),text.data(),length);
        selected->value.size=length; selected->value.revision=++revision_;
        return true;
    }
    std::optional<Value> read(NameKey key) const {
        std::lock_guard lock(mutex_);
        const auto begin=bucket(key);
        for(std::size_t i=0;i<ways;++i)
            if(slots_[begin+i].value.revision && slots_[begin+i].key==key) return slots_[begin+i].value;
        return std::nullopt;
    }
private:
    static constexpr std::size_t ways=4,capacity=2048;
    struct Slot { NameKey key; Value value; };
    static std::size_t bucket(NameKey key) noexcept {
        auto hash=static_cast<std::uint64_t>(key.actor)^(std::uint64_t(key.entity)<<32)^key.epoch;
        hash=(hash^(hash>>30))*0xbf58476d1ce4e5b9ULL;
        hash=(hash^(hash>>27))*0x94d049bb133111ebULL;
        return static_cast<std::size_t>(hash^(hash>>31))%(capacity/ways)*ways;
    }
    mutable std::mutex mutex_;
    std::array<Slot,capacity> slots_{};
    std::uint64_t revision_{};
};
struct TextPoint { float x{},y{}; };
struct NameMesh {
    std::string text;
    std::vector<TextPoint> triangles; // Three points per triangle, normalized to unit height.
    float width{};
};
// Camera-facing label basis in the same world coordinates as the native boxes.
struct NameBillboard { bedrock::Vec3 anchor,right,up; };
inline NameBillboard nameBillboard(const bedrock::ActorBounds& bounds,const bedrock::Vec3& origin) noexcept {
    const bedrock::Vec3 anchor{(bounds.minimum.x+bounds.maximum.x)*0.5f,bounds.maximum.y+0.2f,
        (bounds.minimum.z+bounds.maximum.z)*0.5f};
    auto direction=bedrock::Vec3{origin.x-anchor.x,origin.y-anchor.y,origin.z-anchor.z};
    const float horizontal=std::hypot(direction.x,direction.z);
    const float length=std::sqrt(horizontal*horizontal+direction.y*direction.y);
    if(length<0.0001f) return {anchor,{1,0,0},{0,1,0}};
    if(horizontal<0.0001f) return {anchor,{1,0,0},{0,0,direction.y>0 ? -1.f : 1.f}};
    const bedrock::Vec3 right{direction.z/horizontal,0,-direction.x/horizontal};
    direction={direction.x/length,direction.y/length,direction.z/length};
    return {anchor,right,{direction.y*right.z,direction.z*right.x-direction.x*right.z,
        -direction.y*right.x}};
}
inline bedrock::Vec3 nameVertex(const NameBillboard& b,TextPoint p,float height,
                               const bedrock::Vec3& origin) noexcept {
    return {b.anchor.x+(b.right.x*p.x+b.up.x*p.y)*height-origin.x,
        b.anchor.y+(b.right.y*p.x+b.up.y*p.y)*height-origin.y,
        b.anchor.z+(b.right.z*p.x+b.up.z*p.y)*height-origin.z};
}
} // namespace hitbox
