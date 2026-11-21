#pragma once
namespace bedrock {
enum class BlockActionFailure {
    None, NotLoaded, InvalidFace, GameModeUnavailable, VirtualFunctionMismatch,
    HitContextUnavailable, NativeFault, GameRejected
};
}
