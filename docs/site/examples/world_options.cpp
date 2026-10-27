#include <bedrock/game_api.hpp>

bedrock::PlacementResult placeStone(bedrock::World world, bedrock::BlockPos target) {
    const bedrock::BlockSetOptions options = bedrock::BlockSetBuilder{}
        .slot(bedrock::HotbarIndex{0})
        .orientation(bedrock::Yaw{90.0f}, bedrock::Pitch{0.0f})
        .reach(bedrock::Reach{4.5})
        .build();
    return world.setBlock(target, bedrock::BlockIdentifier{"minecraft:stone"}, options);
}
