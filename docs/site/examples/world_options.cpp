#include <bedrock/detail/block_types.hpp>

bedrock::BlockSetOptions stonePlacementOptions() {
    return bedrock::BlockSetBuilder{}
        .slot(bedrock::HotbarIndex{0})
        .orientation(bedrock::Yaw{90.0f}, bedrock::Pitch{0.0f})
        .reach(bedrock::Reach{4.5})
        .build();
}

#ifdef _WIN32
#include <bedrock/native/world.hpp>

bedrock::PlacementResult placeStone(bedrock::NativeWorld& world, bedrock::BlockPos target) {
    return world.setBlock(target, bedrock::BlockIdentifier{"minecraft:stone"}, stonePlacementOptions());
}
#endif
