#pragma once
#include <cstdint>
#include <cstddef>

namespace bedrock {
enum class CommandOriginType : std::uint8_t {
    Player=0, CommandBlock=1, MinecartCommandBlock=2, DevConsole=3, Test=4,
    AutomationPlayer=5, ClientAutomation=6, DedicatedServer=7, Entity=8,
    Virtual=9, GameArgument=10, EntityServer=11, Precompiled=12,
    GameDirectorEntityServer=13, Scripting=14, ExecuteContext=15
};

enum class CommandResponseStatus { Received, Timeout, Disconnected, Cancelled, InvalidRequest, Unavailable, QueueFull };

enum class CommandSource { Native, Packet };

enum class CommandSubmission { Queued, PairedDuplicate, UnknownCommand, InvalidArguments, Full, Stopped };
} // namespace bedrock
