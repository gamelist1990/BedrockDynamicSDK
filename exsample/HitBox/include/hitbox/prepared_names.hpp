#pragma once
#include "name_tags.hpp"

namespace hitbox {
struct PreparedName {
    std::size_t boxIndex{},firstVertex{},vertexCount{};
};
struct PreparedNames {
    bedrock::Vec3 origin{};
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
    output.origin=origin;
    const auto count=std::min(boxes.size(),names.size());
    std::size_t reserve=0;
    for(std::size_t i=0;i<count;++i) if(names[i])
        reserve+=std::min(maximumVertices-reserve,names[i]->triangles.size()/3*8);
    output.vertices.reserve(reserve);
    output.labels.reserve(count);
    for(std::size_t i=0;i<count;++i) {
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
} // namespace hitbox
