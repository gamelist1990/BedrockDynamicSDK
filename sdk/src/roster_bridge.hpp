#pragma once

// Internal bridge between SDK facades: Hooks::useGameAccess must construct
// its event dispatch around the very same NativeGameAccess a RosterAccess
// wraps, so the access' LevelTick scope opens inside the dispatch's tick
// handlers. Implemented in roster.cpp, where RosterAccess::Impl is
// complete; returns nullptr when the native backend is not in the build.

namespace bedrock {
class NativeGameAccess;
}

namespace bedrock::sdk {
class RosterAccess;
}

namespace bedrock::sdk::detail {
bedrock::NativeGameAccess* nativeGameAccess(RosterAccess& access) noexcept;
} // namespace bedrock::sdk::detail
