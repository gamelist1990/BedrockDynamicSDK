#pragma once
#include <cstdint>
#include <cstddef>

namespace bedrock {
enum class Feature { Input, Gameplay, LevelTick, NameTags, Fov, Keyboard, Actors, Items, PlayerIdentity, World, Motion, Camera,
    PacketSender, PacketWire, MovementPackets, TitlePackets, CommandPackets, ClientCommands, Gui, WorldRender, WorldEvents, WorldHeight, Brightness, GameType };

enum class ProfileSource : std::uint8_t {
    Remote,
    Cache,
    Embedded,
};
} // namespace bedrock
