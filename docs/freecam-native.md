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

## Underground terrain investigation — revision 4

Live revision3 evidence: forcing camera culler type0 causes missing outside terrain and does not provide the requested underground spectator view. Earlier inside-cube suppression ran (`cube_skipped` increased) without fixing the view. Both paths are retired, including direct hooks on permission getter0x9F6630 and cube renderer0x47B0360. No culler setter is invoked by current Freecam. `spectator_mode` remains a compatibility option with unavailable terrain behavior. Gamma remains independent.

Exact executable: Minecraft.Windows.exe1.26.52.3, SHA256 `4a92bfa3ce2428b40ee517b7c1125d9f6f79274382de03846351992663b2e2e4`. REA/Ghidra raw slices use PE .pdata bounds except the leaf getter below. Windows register ABI must be checked against assembly; raw decompiler parameter order is not authoritative.

New static observations:

| RVA | Observed behavior | Evidence / limits |
| --- | --- | --- |
| 0x253D9E0 | Leaf reads ActorGameTypeComponent0x88D3EDDF via EntityContext; returns mode or-1. | PE assembly, RCX=context, EAX=mode. Not a terrain-specific API. |
| 0x9F69B0 | Checks camera PlayerStateAffectsRenderingComponent0x21A688A4. | REA `ev_cef75338b60a81df004fd1aa2f0f5d0449e420fc924ae777b5528b4f793bdd45`. Broad rendering policy, not established spectator terrain switch. |
| 0x4690310 | Reads mode6 / inherited mode5+level6 and selects fog / underwater / underlava variation. | REA `ev_789afa6e4675afafb60ddac2b9962816980d23496ecb61737f5e9aebda5f70ad`. Fog, not terrain visibility. |
| 0x46BD0A0 | Camera fog predicate includes the spectator branch and liquid state+0x971. | REA `ev_b96c63fb6d20090f8f6b8c8df0a191a6c608e642308a9bb3acf45ebebea299a8`. Not selected for hooking. |
| 0x4692740 | Builds actor queues and passes spectator state into a per-actor visitor. | REA `ev_b6f7841e2947100e794d8db2d0d14775d5d968767723866ef1ffe8e1236bdd40`. Does not establish terrain control. |
| 0x46D7E90 | Filters actor queues, including non-spectator observer versus spectator target. | REA `ev_eddc186f3c23eae79ad39e0830731defd9ed402dca0445722a2135544f84e0b3`. Actor/shadow queues, not terrain. |
| 0x4812B00 | Camera visibility-task visitor filters chunk positions by frustum/distance policy. | REA `ev_3047f72b70c033590071cedc625715e3d7605f6d42d905bd9a7041635453d68c`. No confirmed spectator condition. |
| 0x4AFAED0 | Filters nearby actors by radius and excludes spectators. | REA `ev_fa9fd7cac545c57b12063ba6e0742b06119ffe1877f163b7b27c84e7d08cded8`. Not terrain. |

LeviLamina client headers (src-client, not empty server/common placeholders) distinguish ViewRenderData::mDrawInsideCubes, camera fog state, chunk queues and PlayerStateParams::mIsSpectator. Header names are useful leads, not evidence for exact-build offsets or hook ABI. Never globally override the game-type getter: its callers include gameplay/UI and unrelated actor rendering.

Remaining unknown: the exact native path that gives vanilla spectator its underground terrain visibility on the user's renderer. None of these findings is promoted to an ABI-verified terrain symbol. A controlled comparison of ordinary camera and real spectator camera on the same world/render mode is still needed before claiming equivalent rendering.

Evidence is outside Git under `~/Documents/PEXData/REA/freecam-spectator-1.26.52.3/` and `freecam-terrain-1.26.52.3/`. This revision fixes the introduced culling regression; it does not implement underground visibility.


## Revision5 — runtime-correlated spectator culler

The user's comparison recording SHA256 `9dcb928b7838451fcc2467c835c1fe4e1a4143ad6397c6bcf0af180ad1a1ab10` contains76 valid camera samples over37.628s with matching exact-EXE fingerprint. All samples have Freecam OFF and epoch2. It includes two recorded spectator intervals; chat history in the screenshot alone is not a timestamped record of every transition.

| Native Actor GameType | Culler enum | Culler vtable RVA | Samples |
| --- | --- | --- | --- |
| 0 (survival) | 3 | 0xE8E3B10 | 14 |
| 1 (creative) | 3 | 0xE8E3B10 | 20 |
| 6 (spectator) | 5 | 0xE8DFE00 | 19 |
| -1 (unavailable/stale) | 3 or5 | matching mode3/5 table | 23 |

Observed culler transitions relative to the first record:approximately12.0s 3→5,19.1s 5→3,25.1s 3→5,31.1s 5→3 (see retained raw timestamps for exact milliseconds). Unknown GameType samples are not assigned a mode. Raw camera+0x974 toggles within spectator periods as position changes; it is not adopted as a persistent spectator switch.

Static correlation: native factory0x4690B80 case5 dispatches at0x4690E58. It constructs the native cached/async base then installs0xE8DFE00 through RIP-relative LEA at0x46911F0. Vtable slot1 is0x47E40B0; REA/Ghidra evidence `ev_dbb58e05697d13e67c057b0a586e5cdba95b6b93c30d10c6c549deecb6c8b47e` shows scheduling of an async task. The task table0xE8E3EC8 slot2 targets0x4809A70, evidence `ev_15ce07b37b64de3891a74801345de843639d3f7bf7df1bb13c81586737544331`. Do not name this implementation NoneAsync without further semantic evidence.

Implementation uses the guarded native setter0x4690910 (`void __fastcall(LevelRendererPlayer*, unsigned char)`, RCX camera / DL mode). Its already analyzed idle check, native shared_ptr replacement and visibility-cache invalidation remain unchanged. Request5 only during active Freecam with spectator_mode enabled. When busy the native method returns without switching; retry on a later render frame. Only scalar copies cross to workers. No worker accesses native camera/culler pointers.

Lease ownership is tied to camera pointer, SDK world epoch and copied native Actor GameType context. Restore only an owned mode5 lease; discard it across camera/world/native-mode changes. Do not restore an old mode3 over an observed transition into a real spectator. Keep the render callback pinned after Stop for pending restoration. No game-type writes, global spectator predicate override, overlay suppression or mode0 request.

This verifies the *vanilla selection* of mode5 for this capture, not that applying mode5 alone reproduces underground rendering in Freecam. That effect and restoration still require live confirmation. The active profile remains candidate/runtime_verified=false. Logs report selected mode and post-call state separately from visual success.

Retained runtime summary and REA output: `~/Documents/PEXData/REA/freecam-spectator-mode5-1.26.52.3/`.


## Revision6: intercept native pre-render selection and observe view flags

Revision5 runtime logs supplied by the user show ~1600 successful mode3→5 replacements per5s. A mode5 sample alone did not establish stable selection or correct underground rendering. The native preRenderUpdate0x46AA4A0 loads a byte from its parameters+0x4D at0x46AA824 and calls setter0x4690910 at0x46AA82B (return0x46AA830). This is a pre-render method, not a constructor. REA/Ghidra evidence: `ev_fcec2d12337538e3335dbb43d038435de03c4ec30851a447539ac584c29b24ab`.

Revision6 hooks the guarded setter before the original update. Only normal mode3 requests for the current primary camera, SDK epoch and copied native game-mode context are replaced by5 while Freecam's spectator view is active. Other cameras/modes are forwarded unchanged. The bootstrap/restore callback remains; successful switches are limited to8 per activation, then the override backs off. Native requests and actual replacements are counted separately. No worker touches native objects.

A separate read-only observer of camera renderer0x4686F20 forwards all four Windows x64 register arguments unchanged: RCX camera, RDX actor render context, R8 ViewRenderObject, R9 client instance. The function preserves R8, reloads it and tests view+0x80 at0x4688F92 before inside-cube rendering at0x468908E. Evidence: `ev_24a5319a529362441bb8a5489e37fbf08add19d082c53ad8a75f846cbf6c26ac`. The optional diagnostics copy13 bool-sized fields at view+0x7C..0x88, including+0x80, at2Hz. They do not suppress the overlay or write flags. Validity is reported separately; no observation yet establishes the complete spectator terrain policy.

Freecam revision6 needs a new same-world normal/spectator/Freecam comparison. Mode5 stability, camera view flags, underground appearance and restoration remain live-validation tasks. Keep candidate/runtime_verified=false. The user's revision5 capture cannot supply these newly added view fields.


## Revision7: exact primary-camera inside-cube draw gate

The user reports an opaque dirt view except near a block edge after revision6. A screenshot establishes appearance, not hook counters or successful mode5 stability. The extra evidence follows the separate inside-cube gate: camera renderer tests ViewRenderObject+0x80 at0x4688F92 and calls0x47B0360 at0x468908E (return0x4689093). The existing cube-render snapshot shows RCX renderer, RDX client, R8 context preserved, void return.

Producer0x46AC3A0 stores this flag at frame+0x3950 from frame+0x38B8, selected near0x46ACB43..0x46ACBEB. It obtains a layered ability structure through0x1D1930, examines six0xF0 layers starting+0x18 and ability+0xCC (17*12), uses the first non-Unset entry, and negates the bool when present. LeviLamina's AbilitiesIndex identifies17 as NoClip. The layered-ability naming is a header-correlated inference; no live values were supplied for this producer. REA/Ghidra producer evidence `ev_cc9e7a556f28a2128f55f18942897455dc72843125a1f0a2488e885c4b379056`, retained as view-producer-evidence.json. Extract SHA256 d890d3e9ecc42ff796567affe8782b2ea11e39f08e7a10575974bd549962caf8 was matched to exact EXE .pdata body20732bytes. Raw import has external-callee/stack-probe and GCC-signature limitations; Windows register interpretation was checked against PE assembly.

Revision7 leaves the immutable view flags and abilities untouched. A thread-local, nesting-safe scope around the current primary camera permits bypass only at the verified inside-cube call return address, while active Freecam spectator view and the same epoch hold. Other callers forward the original3arguments. This reproduces skipping that draw gate, not every vanilla spectator behavior. No permission hook, game-mode write, full-view memcpy or worker native access is added. New camera/cube/skipped counts establish reachability separately from visual success. Candidate/runtime_verified=false remains.
