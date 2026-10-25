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
and translates the full-size local AABB while detached. On disable it restores position
and full bounds. Native camera/rotation processing remains the game's own;
the previous `_updatePlayer` detour is no longer installed. Legacy camera
profile fields remain for older consumer DLLs.

Runtime 4.2 (same compatible table ABI 4) adds point 14,
`BDS_HOOK_PACKET_SEND_TO_SERVER`, with sender/packet pointers. Freecam derives
the target from the observed local client's sender vtable slot 4, only within
the exact main module's .text and at a .pdata function start. It logs the actual
RVA. The send target is still a **runtime-derived candidate**, not a statically
verified RVA/ABI. Sender observation occurs in native callbacks; the worker
only registers hooks using copied pointer identities. The current middleware
recognizes the exact-build movement packet vtables, captures recent inactive
send positions and requires a fresh copied position before activation. During
flight it anchors packet positions to that pre-flight pose, clears movement
delta/control vectors and movement flags, and forwards the packet. Item action
flags, transactions, tick numbers, acknowledgements and other packet types are
preserved. No packet is stored for replay after M. Native layout failures request
restoration and withhold the affected movement packet.

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

## Interaction forwarding investigation

Exact EXE SHA-256 is recorded above. Static packet candidates and original PE
slices/evidence are in `~/Documents/PEXData/REA/freecam-interaction-1.26.52.3/`.
The active symbol table is unchanged; layouts remain `candidate`, runtime unverified.

- PlayerAuthInput vtable RVA `0xE83A360`, getId `0xB26A00`, validation `0x3C8BA90`.
  REA/Ghidra `ev_0b98d57e71d487b6113586c65b50b93de981c7c1a89563912ddc072c77a676f8`.
  Validation branches refer to original `Validate PlayerAuthInputPacketPayload::mPos`
  at `0xEBE93DA`, `mPosDelta` at `0xECA0828`, `mAnalogMoveVector` at `0xEBFC405`,
  `mMove` at `0xEC53FF3`, and `mRawMoveVector` at `0xEBFC3CF`. These correlate with
  packet offsets `0x38`, `0x48`, `0x5C`, `0x64`, `0x80`. Flags at `0x88` occupy
  16 bytes; bits 34/35/36 are checked against owned data at `0xB0`/`0xC0`/`0xB8`.
  Those flags and owning data must remain paired. The implementation never
  copies/destroys/rebuilds the native transaction, request or block-action objects.
- MovePlayer vtable RVA `0xE85BFC0`, getId `0x2ECD950`, validation `0x2ECE550`.
  REA/Ghidra `ev_9905a373c2eddd0de8ff5f3f373e0ad3cac2f2ab232817382ca317e5b25095e9`.
  Position is validated at `0x38`; normal mode at `0x50` is required for rewriting.
- Field names and flag definitions are corroborated by LeviLamina's
  `PlayerAuthInputPacketPayload.h` and `MovePlayerPacketPayload.h`; they alone
  were not treated as exact-build offset verification. Entry guards and vtable
  getter identity are checked before use. No new native ABI call is introduced.

The reported delayed chest opening is consistent with suppressing the complete
auth-input tick. This host cannot establish the in-game cause or prove that
the new forwarding fixes every case without live logs. Server reach checks and
server inventory authority continue to apply. Nested transaction positions are
left unchanged. The baseline is cleared by world/sender identity and age checks;
vehicle packets and unsupported movement classes cause restoration.

## Local spectator option and full brightness

`native_freecam.visual` remains an exact-build static candidate. The optional
`spectator_mode` (default false) writes only ActorGameTypeComponent's copied
4-byte value to 6 during flight. It saves the previous value and restores it
only for the same world/registry/entity/RuntimeID, and only if the current
value is still 6. An externally changed mode is preserved. This does not call
a Player setter, update server permissions or send a gamemode command.
Inside-block visibility and effects on normal interaction still need live verification.

REA/Ghidra `ev_48dad3a304afd96fd38d97b6cc09b736a9b8fbebf84acbb00bcb8cd6b894c6c2`
for RVA `0x19E0840` observes EnTT hash `0x88D3EDDF`, 4-byte payload indexing,
and a mode-6 success branch. LeviLamina's ActorGameTypeComponent / GameType
declarations corroborate the field and spectator enum.

`full_bright` (default true) hooks the Options getter at RVA `0xFB7880`,
returning 25.0 only while active and otherwise forwarding the original float.
REA/Ghidra `ev_edc97e3e4ab756fe53ae5f3ebbecac083ceaa3129225047afe1358d32180e081`
observes option ID 0x32, option inheritance traversal, and `MOVSS XMM0,[RAX+0x18]`
return. Latite Options_getGamma and Fullbright corroborate meaning and value 25.
No persisted gamma option is written; disabling restores the game's own getter.
An exact entry guard and the shared broker's direct-hook conflict check apply.
Evidence and original slices: `~/Documents/PEXData/REA/freecam-visual-1.26.52.3/`.

[Borion GameModeCommand](https://github.com/Borion-Updated/Borion-OSS/blob/master/Horion/Command/Commands/GameModeCommand.cpp)
accepts modes 0..2 and invokes its historical Player setter. Its offsets/ABI
were not reused for the current executable. The optional local component path
above is independently derived for 1.26.52.3.
