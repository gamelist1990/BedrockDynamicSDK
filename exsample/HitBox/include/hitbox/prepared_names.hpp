#pragma once
#include "name_tags.hpp"

namespace hitbox {
struct PreparedName {
    std::size_t boxIndex{},firstVertex{},vertexCount{};
};
struct PreparedNames {
    bedrock::Vec3 origin{};
    Clock::time_point preparedAt{};
    float height{};
    std::vector<bedrock::Vec3> vertices;
    std::vector<PreparedName> labels;
    std::size_t budgetSkipped{};
};
// Worker-only geometry. Native rendering just rebases the tessellator once per
// snapshot and submits these immutable vertices; no per-glyph billboard math.
inline PreparedNames prepareNames(const std::vector<BoxRecord>& boxes,
    const std::vector<std::shared_ptr<const NameMesh>>& names,
    const bedrock::Vec3& origin,Clock::time_point now,float height,std::size_t maximumVertices) {
    PreparedNames output;
    output.origin=origin; output.preparedAt=now; output.height=height;
    const auto count=std::min(boxes.size(),names.size());
    std::size_t reserve=0;
    for(std::size_t i=0;i<count;++i) if(names[i])
        reserve+=std::min(maximumVertices-reserve,names[i]->triangles.size()/3*8);
    output.vertices.reserve(reserve);
    output.labels.reserve(count);
    // Saved Player labels must not be permanently starved behind live Actor
    // meshes in unordered-map iteration order when the vertex budget fills.
    std::vector<std::size_t> order;
    order.reserve(count);
    for(std::size_t i=0;i<count;++i) order.push_back(i);
    std::sort(order.begin(),order.end(),[&](std::size_t a,std::size_t b) {
        if(boxes[a].retained!=boxes[b].retained) return boxes[a].retained>boxes[b].retained;
        if(boxes[a].isPlayer!=boxes[b].isPlayer) return boxes[a].isPlayer>boxes[b].isPlayer;
        const auto da=distanceSquared(center(boxes[a].bounds),origin);
        const auto db=distanceSquared(center(boxes[b].bounds),origin);
        return da==db ? boxes[a].runtimeId<boxes[b].runtimeId : da<db;
    });
    for(const auto i:order) {
        const auto* mesh=names[i].get();
        if(!mesh || mesh->triangles.empty() || mesh->triangles.size()%3) continue;
        const auto needed=mesh->triangles.size()/3*8;
        if(needed>maximumVertices-output.vertices.size()) { ++output.budgetSkipped; continue; }
        const auto first=output.vertices.size();
        const auto billboard=nameBillboard(interpolatedBounds(boxes[i],now),origin);
        for(std::size_t p=0;p<mesh->triangles.size();p+=3) {
            const auto a=nameVertex(billboard,mesh->triangles[p],height,origin);
            const auto b=nameVertex(billboard,mesh->triangles[p+1],height,origin);
            const auto c=nameVertex(billboard,mesh->triangles[p+2],height,origin);
            for(const auto& vertex:std::array<bedrock::Vec3,8>{a,b,c,c,c,b,a,a})
                output.vertices.push_back(vertex);
        }
        output.labels.push_back({i,first,needed});
    }
    return output;
}
// Compare cheap scalar keys before allocating/sorting/transforming glyphs.
// Bounds tolerance is 1mm; camera-origin tolerance is 1cm. Both are measured
// against the original prepared geometry, so repeated reuse cannot accumulate drift.
inline bool canReuseNames(const PreparedNames& prepared,
    const std::vector<BoxRecord>& oldBoxes,const std::vector<std::shared_ptr<const NameMesh>>& oldNames,
    const std::vector<BoxRecord>& boxes,const std::vector<std::shared_ptr<const NameMesh>>& names,
    const bedrock::Vec3& origin,Clock::time_point now,float height) noexcept {
    if(prepared.height!=height || distanceSquared(prepared.origin,origin)>0.01f*0.01f ||
        oldBoxes.size()!=boxes.size() || oldNames.size()!=oldBoxes.size() ||
        oldNames.size()!=names.size() || names.size()!=boxes.size()) return false;
    const auto near=[](const bedrock::Vec3& a,const bedrock::Vec3& b) {
        return std::abs(a.x-b.x)<=0.001f && std::abs(a.y-b.y)<=0.001f && std::abs(a.z-b.z)<=0.001f;
    };
    for(std::size_t i=0;i<boxes.size();++i) {
        if(oldBoxes[i].runtimeId!=boxes[i].runtimeId || oldBoxes[i].retained!=boxes[i].retained ||
            oldBoxes[i].isPlayer!=boxes[i].isPlayer || oldNames[i].get()!=names[i].get()) return false;
        if(!names[i]) continue;
        const auto previous=interpolatedBounds(oldBoxes[i],prepared.preparedAt);
        const auto current=interpolatedBounds(boxes[i],now);
        if(!near(previous.minimum,current.minimum) || !near(previous.maximum,current.maximum)) return false;
    }
    return true;
}
} // namespace hitbox
