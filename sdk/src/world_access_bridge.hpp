#pragma once

// Internal bridge between SDK facades: some facades (BlockActions) must be
// bound to the very same native world accessor the Mod's WorldAccess facade
// wraps, because the native block-action adapter scopes its hit context and
// GameMode reads through that instance. Implemented in world_access.cpp,
// where WorldAccess::Impl is complete; returns nullptr when the native
// world backend is not part of the build.

namespace bedrock::sdk {
class WorldAccess;
}

namespace bedrock::sdk::detail {
void* nativeWorldAccessHandle(WorldAccess& access) noexcept;
} // namespace bedrock::sdk::detail
