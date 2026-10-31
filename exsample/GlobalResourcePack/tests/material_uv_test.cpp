#include <bedrock/detail/material_uv_patch.hpp>
#include <cassert>
#include <limits>
using namespace bedrock::detail;
static void put(std::vector<std::byte> &b, std::size_t offset, float value) {
    std::memcpy(b.data() + offset, &value, sizeof(value));
}
int main() {
    MaterialUvLayout layout{96,24,4,0,92,0,16,20,22};
    std::vector<std::byte> source(192), original(48, std::byte{0xA5}), changed;
    for (std::size_t row=0; row<2; ++row) {
        auto s=row*96;
        put(source,s, row ? 0.75f : 0.25f);
        put(source,s+4,0.1f); put(source,s+8,0.2f);
        put(source,s+12,0.5f); put(source,s+16,0.8f);
        source[s+92]=std::byte{0x34}; source[s+93]=std::byte{0x12};
    }
    const auto untouched = original;
    assert(planMaterialUvs(layout,source,original,changed));
    assert(original==untouched); // Planner never touches the live snapshot.
    for (std::size_t row=0; row<2; ++row) {
        auto d=row*24;
        float value{}; std::memcpy(&value,changed.data()+d,4); assert(value==0.1f);
        std::memcpy(&value,changed.data()+d+16,4); assert(value==(row ? 0.75f : 0.25f));
        assert(changed[d+20]==std::byte{0x34} && changed[d+21]==std::byte{0x12});
        assert(changed[d+22]==std::byte{0xA5} && changed[d+23]==std::byte{0xA5});
    }
    auto packA=changed;
    put(source,4,0.6f); put(source,12,0.9f);
    assert(planMaterialUvs(layout,source,changed,changed)); // A -> B
    assert(changed!=packA);
    put(source,4,0.1f); put(source,12,0.5f);
    assert(planMaterialUvs(layout,source,changed,changed)); // B -> A
    assert(changed==packA);
    const auto good=changed;
    source.resize(96); // Variant change cannot resize a live game vector.
    assert(!planMaterialUvs(layout,source,changed,changed) && changed==good);
    source.resize(192);
    put(source,4,std::numeric_limits<float>::quiet_NaN());
    assert(!planMaterialUvs(layout,source,changed,changed) && changed==good);
    put(source,4,0.7f); put(source,12,0.3f); // Inverted UV bounds.
    assert(!planMaterialUvs(layout,source,changed,changed) && changed==good);
    layout.sourcePbr=SIZE_MAX; // Corrupt profile cannot overflow bounds check.
    assert(!planMaterialUvs(layout,source,changed,changed) && changed==good);
}
