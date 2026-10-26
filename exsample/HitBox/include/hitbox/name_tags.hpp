#pragma once
#include "box_cache.hpp"
#include <cstring>
#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

namespace hitbox {
// Copied identity only. Workers never dereference an Actor handle.
struct NameKey {
    std::uint64_t epoch{};
    std::uint64_t runtimeId{};
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
    struct Value { std::array<char,maximumBytes> bytes{}; std::size_t size{}; std::uint64_t revision{}; Clock::time_point observedAt{}; };
    bool observe(NameKey key,std::string_view text) noexcept {
        if(!key.epoch || !key.runtimeId || !key.actor || text.empty() || !hasVisibleName(text)) return false;
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock) return false;
        auto* slot=&slots_[bucket(key)];
        auto* selected=slot;
        for(std::size_t i=0;i<ways;++i) {
            if(slot[i].value.revision && sameRuntime(slot[i].key,key)) { selected=&slot[i]; break; }
            if(!slot[i].value.revision || slot[i].value.revision<selected->value.revision) selected=&slot[i];
        }
        const auto length=std::min(text.size(),maximumBytes);
        if(selected->value.revision && sameRuntime(selected->key,key) && selected->value.size==length &&
           !std::memcmp(selected->value.bytes.data(),text.data(),length)) {
            selected->value.observedAt=Clock::now();return false;
        }
        selected->key=key;
        std::memcpy(selected->value.bytes.data(),text.data(),length);
        selected->value.size=length; selected->value.revision=++revision_;
        selected->value.observedAt=Clock::now();
        publishedRevision_.store(revision_,std::memory_order_release);
        return true;
    }
    // A busy worker means defer the optional native lookup, never block the game.
    bool cachedOrBusy(NameKey key,std::chrono::minutes lifetime=std::chrono::minutes(10)) const noexcept {
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock) return true;
        const auto begin=bucket(key);
        for(std::size_t i=0;i<ways;++i)
            if(slots_[begin+i].value.revision && sameRuntime(slots_[begin+i].key,key) && Clock::now()-slots_[begin+i].value.observedAt<lifetime) return true;
        return false;
    }
    std::uint64_t revision() const noexcept { return publishedRevision_.load(std::memory_order_acquire); }
    std::optional<Value> read(NameKey key) const {
        std::lock_guard lock(mutex_);
        const auto begin=bucket(key);
        for(std::size_t i=0;i<ways;++i)
            if(slots_[begin+i].value.revision && slots_[begin+i].key==key) return slots_[begin+i].value;
        return std::nullopt;
    }
    // Actor pointers and EnTT generations can change while Bedrock keeps the
    // same RuntimeID. Reuse the last observed name in that case.
    std::optional<Value> readForRuntime(NameKey key) const {
        std::lock_guard lock(mutex_);
        const auto begin=bucket(key);
        const Slot* match=nullptr;
        for(std::size_t i=0;i<ways;++i) {
            const auto& slot=slots_[begin+i];
            if(slot.value.revision && sameRuntime(slot.key,key) &&
               (!match || slot.value.revision>match->value.revision)) match=&slot;
        }
        return match ? std::optional<Value>{match->value} : std::nullopt;
    }

private:
    static constexpr std::size_t ways=4,capacity=2048;
    struct Slot { NameKey key; Value value; };
    static bool sameRuntime(NameKey a,NameKey b) noexcept {
        return a.epoch==b.epoch && a.runtimeId==b.runtimeId;
    }
    // Hidden/format-only getters must not replace the last usable name before
    // a worker has consumed it. Do this without allocation on the native hook.
    static bool hasVisibleName(std::string_view text) noexcept {
        for(std::size_t i=0;i<text.size();) {
            const auto c=static_cast<unsigned char>(text[i++]);
            if(c=='\n'||c=='\r') return false;
            if(c==0xc2 && i<text.size() && static_cast<unsigned char>(text[i])==0xa7) {
                ++i;if(i<text.size())++i;continue;
            }
            if(c>32 && c!=127) return true;
        }
        return false;
    }
    static std::size_t bucket(NameKey key) noexcept {
        auto hash=key.epoch^key.runtimeId;
        hash=(hash^(hash>>30))*0xbf58476d1ce4e5b9ULL;
        hash=(hash^(hash>>27))*0x94d049bb133111ebULL;
        return static_cast<std::size_t>(hash^(hash>>31))%(capacity/ways)*ways;
    }
    mutable std::mutex mutex_;
    std::array<Slot,capacity> slots_{};
    std::uint64_t revision_{};
    std::atomic<std::uint64_t> publishedRevision_{};
};
struct TextPoint { float x{},y{}; };
struct NameMesh {
    std::string text;
    std::vector<TextPoint> triangles; // Three points per triangle, normalized to unit height.
    float width{};
    std::vector<std::array<TextPoint,4>> quads; // Convex pairs; unmatched triangles use a repeated corner.
};

// Run once per acquired text on a worker. Merge only convex adjacent triangles;
// holes, concave outlines and unpaired triangles retain their original coverage.
inline void packNameQuads(NameMesh& mesh) {
    mesh.quads.clear();
    if(mesh.triangles.size()%3) return;
    struct Edge { TextPoint a,b,tip; std::size_t triangle; };
    const auto less=[](TextPoint a,TextPoint b) { return std::tie(a.x,a.y)<std::tie(b.x,b.y); };
    const auto equal=[](TextPoint a,TextPoint b) { return a.x==b.x && a.y==b.y; };
    const auto cross=[](TextPoint a,TextPoint b,TextPoint c) {
        return (b.x-a.x)*(c.y-b.y)-(b.y-a.y)*(c.x-b.x);
    };
    const auto count=mesh.triangles.size()/3;
    std::vector<Edge> edges; edges.reserve(count*3);
    std::vector<bool> used(count);
    mesh.quads.reserve(count);
    for(std::size_t i=0;i<count;++i) for(std::size_t j=0;j<3;++j) {
        auto a=mesh.triangles[i*3+j],b=mesh.triangles[i*3+(j+1)%3];
        if(less(b,a)) std::swap(a,b);
        edges.push_back({a,b,mesh.triangles[i*3+(j+2)%3],i});
    }
    std::sort(edges.begin(),edges.end(),[&](const Edge& a,const Edge& b) {
        if(!equal(a.a,b.a)) return less(a.a,b.a);
        return less(a.b,b.b);
    });
    for(std::size_t i=0;i<edges.size();) {
        auto end=i+1;
        while(end<edges.size() && equal(edges[i].a,edges[end].a) && equal(edges[i].b,edges[end].b)) ++end;
        if(end-i==2) {
            const auto& a=edges[i]; const auto& b=edges[i+1];
            if(a.triangle!=b.triangle && !used[a.triangle] && !used[b.triangle]) {
                const std::array<TextPoint,4> quad{a.tip,a.a,b.tip,a.b};
                const float first=cross(quad[0],quad[1],quad[2]);
                bool convex=std::abs(first)>1e-10f;
                for(std::size_t j=1;j<4;++j)
                    convex=convex && cross(quad[j],quad[(j+1)%4],quad[(j+2)%4])*first>0;
                if(convex) { mesh.quads.push_back(quad); used[a.triangle]=used[b.triangle]=true; }
            }
        }
        i=end;
    }
    for(std::size_t i=0;i<count;++i) if(!used[i])
        mesh.quads.push_back({mesh.triangles[i*3],mesh.triangles[i*3+1],mesh.triangles[i*3+2],mesh.triangles[i*3+2]});
}
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
