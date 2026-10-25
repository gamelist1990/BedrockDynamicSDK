# Freecam native investigation (1.26.52.3)

Target SHA-256: `4a92bfa3ce2428b40ee517b7c1125d9f6f79274382de03846351992663b2e2e4`.

The user supplied [Horion's 1.18.12 Freecam](https://github.com/marioCST/Horion/blob/1.18.12/Horion/Module/Modules/Freecam.cpp).
It disables camera position copies with NOPs. This example uses a guarded
function detour instead of carrying those old instruction patches forward.
No Horion source is copied into this example.

## Camera propagation hook

Latite's `Signatures::_updatePlayer` pattern matches exactly once at RVA
`0x6733360`, `.pdata` range `[0x6733360,0x6733BCC)`.
Reference: `src/mc/Addresses.h` and `GenericHooks::hkUpdatePlayer` in
[Latite](https://github.com/LatiteClient/Latite).

REA/Ghidra evidence:
`ev_3af7807da2aedee3d05efa046feb07e3e732e83205bf7083c8bc6317ba5eea54`.
The assembly consumes RCX's quaternion at +0x30, RDX's LookMode integer and
R8's Actor context (+0x10/+0x18), then writes the Actor rotation through +0x228.
The Windows prototype is `void(CameraComponent*, UpdatePlayerFromCameraComponent*, Actor*)`.
Generic raw x64 Ghidra pseudocode parameter ordinals are not the Windows ABI.
CameraComponent's +0x40 position and +0x30 quaternion agree with Latite and
LeviLamina declarations. Position consumption by the later render pipeline
still needs validation in a live game. The mod logs its candidate status.

The hook forwards all calls while inactive and all non-local Actors. During
Freecam it changes the camera component and skips camera-to-local-Player
rotation propagation. It never overwrites Actor position or crafts movement
packets. Body gravity, server corrections and damage continue normally.

IClientInstance vtable 31 returns the local Actor (`0x5DAED30`). It is hooked
passively to bind the actual local Actor; a generic camera Actor is never
assumed to be the local Player. Bootstrap requires the game to invoke this
getter and the cursor gameplay hook (Esc, then resume).

## Damage

`MobHurtTimeComponent` FNV-1a hash: `0xD7A3585C`; 4-byte integer at +0.
REA evidence:
`ev_3ba434a291ee097445d537a2a44bfd5da4aff7174516b7e594ac8a183791ab21`.
Exact PE function `[0x20724E0,0x207275D)` resolves that hash and calculates
4-byte component payload elements. LeviLamina declares it as IntComponent.
The feature refuses activation if the required local hurt value is unavailable,
and exits on an increase; it is not a guarantee of observing a counter reset
and increase entirely between two camera callbacks.

## Rendering and cost

Native reads are restricted to game callbacks. Each camera callback examines
at most 8 type-pool rows by default (hard maximum 16), with a cooperative 150us
budget. One guarded read can exceed that budget. Only Player ActorType 319 is
accepted. RuntimeID and AABB scalars go to a bounded, latest-only mailbox.
One worker handles distance filtering, cache expiry, vertices, configuration
reload and logs. Render callbacks submit one line mesh, at most 128 Players,
24 vertices per Player. Names, inventories and item display names are not read.

SDK runtime 4.0.0 introduces shared render point `BDS_HOOK_RENDER_LEVEL` so the
new Freecam and updated HitBox can use the same native render method. All
co-loaded mods must be rebuilt with runtime 4; old runtime owners require a
complete game restart. HitBox now subscribes rather than installing a
competing direct detour. The reusable native render profile parser lives in
`include/bedrock/native_world_render.hpp`.

Exact executable fingerprint and entry bytes are checked before installation.
All new Freecam offsets and targets remain `candidate`, `runtime_verified:false`.
Evidence/slices are outside the repository in
`~/Documents/PEXData/REA/freecam-1.26.52.3/`. Static inspection does not establish
live camera movement, native rendering or interaction correctness.
