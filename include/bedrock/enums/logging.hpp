#pragma once
#include <cstdint>
#include <cstddef>

namespace bedrock {
enum class LogLevel : unsigned char { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };
} // namespace bedrock
