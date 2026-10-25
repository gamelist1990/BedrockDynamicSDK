# Freecam native investigation (1.26.52.3)

Target SHA-256: `4a92bfa3ce2428b40ee517b7c1125d9f6f79274382de03846351992663b2e2e4`.

The user supplied [Horion's 1.18.12 Freecam](https://github.com/marioCST/Horion/blob/1.18.12/Horion/Module/Modules/Freecam.cpp).
It disables camera position copies with NOPs. The earlier prototype explored
a guarded function detour. The current Borion backend is described below.
No Horion source is copied into this example.

## Earlier camera propagation investigation (not installed)

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

That earlier hook forwards all calls while inactive and all non-local Actors. During
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

Native reads are restricted to game callbacks. Each LevelTick examines
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

## Borion backend (current)

The current backend follows
[Borion Freecam](https://github.com/Borion-Updated/Borion-OSS/blob/master/Horion/Module/Modules/Freecam.cpp)
and the corresponding `Memory/Hooks.cpp` send middleware. It stores the local
Player's original position/AABB, integrates yaw-relative horizontal movement
on LevelTick, ascends/descends 0.5 blocks/tick, zeros velocity/fall distance,
and collapses the local AABB while detached. On disable it restores position
and full bounds. Native camera/rotation processing remains the game's own;
the previous `_updatePlayer` detour is no longer installed. Legacy camera
profile fields remain for older consumer DLLs.

Runtime 4.2 (same compatible table ABI 4) adds point 14,
`BDS_HOOK_PACKET_SEND_TO_SERVER`, with sender/packet pointers. Freecam derives
the target from the observed local client's sender vtable slot 4, only within
the exact main module's .text and at a .pdata function start. It logs the actual
RVA. The send target is still a **runtime-derived candidate**, not a statically
verified RVA/ABI. Sender observation occurs in native callbacks; the worker
only registers hooks using copied pointer identities. The middleware recognizes
constant native `getId` leaves, blocking MovePlayer (19) and PlayerAuthInput
(144) only while the proxy needs suppression. No movement packets are queued,
modified or replayed. Before enabling, it must observe a recognized movement
packet. An unrecognized packet getter while active requests restoration and
does not forward that unknown packet until the proxy is restored.

`IClientInstance::getPacketSender` is slot 293 of the anchored exact client
table. RVA 0x80B340 is exactly `mov rax,[rcx+0x1D0]; ret`:
REA `ev_e12438785166ca79cf685b1ba8f4f3e8331a288338a9d20d765b284e9cf0d336`.
`FallDistanceComponent` hashes to 0xCE6B34F6. Its exact type descriptor is
observed by REA `ev_eb6be23724ba01a4b27aead0d65421d15d6da91c08fe93b8b6c121c1c26e308c`.
That descriptor corroborates the hash, **not** the float payload offset or write
semantics. StateVector current/previous/velocity offsets 0/12/24 and fall value
+0 agree with current LeviLamina declarations and remain runtime candidates.

Restoration checks world epoch, local Actor, registry pointer, EnTT generation
and RuntimeID. Leave/cancel callbacks attempt restoration before teardown.
Post-join/dimension callbacks discard the old epoch without writing old world
coordinates into the new one. After Stop, pinned tick callbacks may perform
pending restoration; other operations become pass-through/no-op. This is not
validated in a live Minecraft process on this host.

## Freecam Player boxes: per-tick capture

Player discovery examines at most `capture_rows_per_update` type-pool rows per
LevelTick (default 8, maximum 16), with the existing cooperative discovery budget.
A fixed array of at most 128 known Player handles is refreshed each game tick;
entity generation, RuntimeID and owner/self checks precede each AABB copy.
This bounded refresh is separate from discovery and does not claim a hard 150us
limit for the entire tick. No name or inventory APIs are involved.
Workers prepare paired previous/current line vertices from copied AABBs. Render
callbacks interpolate these scalar vertices over 50ms from the capture timestamp.
Gaps above 250ms and jumps above 8 blocks snap. Missing Actors are removed on
the next valid capture; stale snapshots expire after 250ms.
