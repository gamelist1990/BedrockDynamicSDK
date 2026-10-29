#pragma once
#include <cstdint>
#include <cstddef>

namespace bedrock {
enum class CameraChange { Applied, NoPlayer, WrongThread, WorldChanged, InvalidValue, NativeFailure };
} // namespace bedrock
