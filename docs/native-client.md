# Native client access

`bedrock::client` is an opt-in Windows x64 MSVC SDK target. Enable
`BEDROCK_SDK_ENABLE_NATIVE_CLIENT` before adding the SDK through FetchContent.
The dependency on the exact EnTT revision is private to the SDK implementation.

Create one `NativeGameAccess` and pass it to `GameApi`. Load it synchronously
from `ProfileManager::prepare` before installing typed `GameHooks`. The adapter
checks required layouts and exact entry guards; a rejected cached definition
can then fall back to the bundled exact-build profile. A packet-era definition
without item-name ownership metadata is rejected rather than partially enabled.

`GameHooks::installGameplay` supplies the client identity. `GameApi` brackets
LevelTick callbacks with `beginWorldTick` / `endWorldTick`, including exceptional
listener exits. NativeGameAccess rejects other threads and, where the profile
provides ClientInstance::getLevel, ticks from a different client world. Do not
retain native handles for use by a worker.

Use `equipmentSnapshot` to distinguish an unavailable read from an empty slot.
The returned item, roster and face data are copied values. Roster and NameTag
providers must also return copied snapshots, never borrowed game storage.
`rosterSnapshot` uses the packet provider when supported, otherwise the SDK's
versioned native PlayerList reader. Worker and render code should consume only
these copied values.

The adapter invokes the profile's item hover-name method with a game-constructed
Release MSVC string buffer, copies its contents, and frees its allocation with
the game deallocator. It never lets the consuming DLL's CRT destroy that string.
Profiles without an ownership binding disable display names; 1.26.52.3 currently
uses item IDs. Explicit candidate policy is still required for unverified ABIs.

World identity changes must invalidate application caches and call
`GameApi::clearObservedActors` to allow unchanged names to be observed again.
`clearWorldCaches` clears native item caches only during an accepted tick.
The adapter and hook callback owners must outlive all installed subscriptions.
See `exsample/TabList` for a complete example with copied-data workers.
