# Diet — surfaces, gaps and verification

Target: 1.26.65.1 (`data/1.26.65.1/windows-x64/manifest.json`, exe sha256
`dacd67e7…093b88`). Evidence for each claim lives outside the repo in
`~/Documents/PEXData/diet/` (see `evidence-13-render-surface.md` for the per-actor queue).

Phase 2 added the F3 debug screen (`docs/diet-f3.md`), fixed the frame-time
guard so a uniformly slow machine is not starved of culling work, and extended
the static analysis of the render and packet surfaces (evidence-04/05).
Phase 3 profiled the RenderDragon extraction predicate (probe-only: its objects
carry no actor identity) and the packet handler map. Phase 4 wired packet
deferral on the three shared_ptr `UpdateBlock`-family handler entries, verified
it live on BOL. Phase 5 added the per-actor camera-queue skip
(`queueRenderEntities`, evidence-13), extended deferral to the two chunk classes
`LevelChunkPacket` (58) and `UpdateSubChunkBlocksPacket` (172), and measured the
combined chunk burst on the local dedicated server (evidence-14). The smoothing
task closes as a documented negative: the correction and movement handlers are
identified, but the game already owns a `MovementInterpolatorSystem` and the
only Mod-side writes would be authoritative state (evidence-15).

## Surfaces Diet builds on (all confirmed)

| Need | Surface | Notes |
| --- | --- | --- |
| block read at (x,y,z) | `sdk::WorldAccess::getBlock` → `BlockSource::getBlock` (`0x32CE180`, verified) | returns `Cell{identifier, kind, properties, supportsPlacement}`; solidity is the native `BlockType.flags & 2` read at `+0x164` |
| actor enumeration + AABB | `sdk::ActorFacade::capture/bounds/position` (EnTT storage intersection) | value copies only; runtimeId identity |
| local player / eye position | `ActorFacade::localPlayer` + `WorldAccess::pose` | camera = position + 1.62 |
| tick | `sdk::Hooks::onLevelTick` | native reads stay on the game thread |
| render frame clock + overlay | `sdk::WorldRender::onPostRender` | read-only snapshot consumption |
| skip an occluded actor's draw | `sdk::ActorRenderFilter` → `queueRenderEntities` lambda 156 (`0x53EA400`) | The queue calls this once per candidate with the `Actor*`. The SDK checks the entry, callable slots and type descriptor. Diet can omit only hidden non-player keys, behind `cull_skip_draw`. |
| Present-thread text/overlay drawing | `sdk::Overlay` (ImGui foreground draw list) | F3 screen; render-only, no input capture |
| incoming packet observation | `sdk::PacketObserver` (new, wraps `PacketAPI::onAnyReceive`) | post-decode, networking thread, payload size only |
| local player rotation | `WorldAccess::component` + `readField` on the actor rotation component | F3 facing line |
| settings | `sdk::Mod` + `sdk::SettingsPage` | persisted by the Mod Settings store |

## Surfaces Diet needs and does not have

| Need | Missing surface | Why it blocks |
| --- | --- | --- |
| defer delay-tolerant packets | **done** — `sdk::PacketDeferral` at the concrete handler entries | five entries are pinned by RVA + entry guard, by the `lea` of their own queued-lambda callable table and by their vtable slot (`0xF2D1E20`: 110/21/56/58/172 → 228/229/192/189/67). `UpdateBlock`, `UpdateBlockSynced`, `BlockActorData` and `LevelChunk` are by-value `shared_ptr` entries; `UpdateSubChunkBlocks` has no shared_ptr entry, so its typed dispatcher (`0x23A83B0`) is detoured instead and gated by the packet object's vtable. A held packet is replayed from the game thread under `net_defer_budget` (evidence-14). |
| smooth remote entities / blend local corrections | a per-actor render transform hook, or a correction-handling entry | the game's own `MovementInterpolatorSystem` already interpolates (`tickClient`, `copyInterpolateComponent`, `RenderPositionComponent`); the only Mod-side points are packet rewrites or writes to `StateVectorComponent`, which are authoritative state and forbidden. The local correction is `CorrectPlayerMovePredictionPacket` (161) handler `0x6BB1C30`; it is server-only, so BOL single player cannot exercise a blend. No safe write surface was found (evidence-15). |
| native full-cube/opaque query | e.g. `Block::getCollisionShape`/opacity | `Cell` exposes the solid flag, not the render shape; Diet uses a conservative identifier whitelist instead. |

## Exact/opaque classification rule

`isOpaqueBlockName()` in `src/diet.cpp`:

1. air/cave_air/void_air → not occluding;
2. `leaves` → occluding only when `cull_leaves` is on;
3. any name containing slab/stairs/fence/wall/pane/door/trapdoor/glass/ice/
   water/flora/etc. → not occluding;
4. otherwise occluding only when the name matches the full-cube whitelist
   (stone/dirt/sand/planks/log/wood/deepslate/…).

Unknown names are **not** occluders, so the engine errs towards keeping an
entity visible. Combined with `hideAfter` hysteresis and "show on any visible
sample", a wrong verdict costs frames, never a wrongly hidden mob.

## Render settings

| id | default | effect |
| --- | --- | --- |
| `cull_skip_draw` | on | lets Diet skip a matching hidden non-player actor's submission when the SDK filter installs |
| `cull_probe` | off | logging only: measures matches without skipping |

## Settings added in phase 4

| id | default | effect |
| --- | --- | --- |
| `net_defer` | on | kill switch for packet deferral: off ⇒ every intercepted handler runs immediately |
| `net_defer_budget` | 8 | held packets returned per level tick (1–64) |

## Packet deferral (phase 4, extended in phase 5)

`sdk::PacketDeferral` (`internal/include/bedrock/native/packet_deferral.hpp`,
`sdk/src/packet_deferral.cpp`) detours five profiled entries: the concrete
`LegacyClientNetworkHandler::handle(NetworkIdentifier const&, shared_ptr<…>)`
overloads for `UpdateBlock` (21), `UpdateBlockSynced` (110), `BlockActorData`
(56) and `LevelChunk` (58), plus the `UpdateSubChunkBlocks` (172) typed
dispatcher. A handler is only held when the Mod policy (`diet::mayDefer`) marks
its protocol id delay-tolerant; everything else runs immediately. Install
verifies, before hooking anything:

1. the exact entry bytes at the profiled RVA (40-byte guard);
2. that the handler body contains the `lea` of its own queued-lambda callable
   table (pairs the address with the overload); the LevelChunk (58) and
   UpdateSubChunkBlocks (172) references lie outside the first 0x200 bytes, so
   their profile rows pin the exact offset;
3. that the handler is the target of vtable `0xF2D1E20` at the profiled slot;
4. that the control-block layout is the one the handler's own epilogue uses
   (`uses` at +8, `weaks` at +0xC);
5. for 172 only: that the typed dispatcher singleton `0x123EEA70` exists and
   its vtable `0xF13A9A0` slot 1 is the profiled `0x23A83B0`, and that
   `0x23A83B0` is the packet class's own dispatcher (its factory is the only
   module reference to that instance).

At run time every call re-checks `[this] == module + 0xF2D1E20`; a call from any
other receiver goes straight to the original (`defer_other_object`). The 172
detour additionally checks the packet object's vtable against `0xF154BB0`
(`defer_class_filtered`), so a dispatcher shared by another packet type could
never be queued as 172.

Held packets keep the by-value `shared_ptr` reference the handler would have
consumed, plus a 0xB0-byte copy of the `NetworkIdentifier` (the handler itself
memcpys that object, so it is trivially copyable). The 172 dispatcher borrows
rather than consumes its caller's pair, so the adapter takes its own use count
and releases it after replay. `onLevelTick` returns at most `net_defer_budget`
per tick in FIFO order; anything older than 400 ms is drained regardless of the
budget; a world change drops the queue (`defer_world_drops`) because held
updates belong to the level being left.

Log fields: `deferred=`, `replayed=`, `queue=`, `defer_calls=`,
`defer_overflow=`, `defer_other_object=`, `defer_class_filtered=`,
`defer_aged=`, `defer_world_drops=`, `defer_total=`, and per-class
`id58_calls/held/replayed`, `id172_calls/held/replayed`; F3 shows `deferred=`
and `queue=` on the packets line.

Measurement (HomePC dedicated server, same 10 086-block `/fill` burst and a
fresh-area teleport, evidence-14 §5): deferral held and replayed every chunk
packet (275 LevelChunk, 32 UpdateSubChunkBlocks over the burst windows) with no
overflow, aged-out or world drop. `frame_ms p50/p95` were unchanged (≈16.0 /
≈20 ms in both runs); the peak frame of the teleport windows was 41.25 ms with
deferral off and 46.36 ms with it on. The burst cost is therefore chunk
decode/generation and mesh work, not the handler invocation this surface moves;
the feature is correct and bounded but is not a frame-time fix by itself.


## Smoothing (phase 5, documented only)

`CorrectPlayerMovePredictionPacket` (161) is the server correction: typed
dispatcher `0x123EEA10`/`0x23A8230` → handler slot 76 (`0x6BB1C30`) →
`0x10C9ED0` → `0x10C9910`. `MovePlayerPacket` (19), `MoveActorDeltaPacket`
(111) and `SetActorMotionPacket` (40) follow the same typed-dispatch pattern
(`0x6BD4990`, `0x6BD4DF0`, `0x6BD7200`). All of them apply through ECS state
(`StateVectorComponent` and the actor update path), and the game already owns a
`MovementInterpolatorSystem` (`tickClient` / `copyInterpolateComponent` /
`RenderPositionComponent`). A packet rewrite can only scale one correction, not
blend it across frames, and no per-actor render-transform surface exists. No
smoothing code or setting was added; the full evidence is
`evidence-15-smoothing.md`.

## Budget and threading

- Actor capture: `ActorFacade::capture` incremental cursor (96 rows/tick) with
  the facade's own cooperative budget.
- Raycasts: `OcclusionEngine` stops before starting any check once
  `tickBudgetUs` (default 700 µs) is exhausted; a partially completed target is
  left unchanged (never hidden on partial evidence).
- Per-target schedule: 120/300/600 ms by distance band; no target is evaluated
  every frame.
- Frame guard: a frame slower than `max(net_budget_ms, 2.5 × the smoothed frame
  time)` defers **all** Diet work. The smoothed baseline matters: a machine that
  runs at 100 ms/frame (software rendering) is not a permanent stall, so the
  guard must not starve culling there. Before phase 2 the guard compared
  against `net_budget_ms` alone (3 ms default), which deferred every tick on
  every machine below 333 fps — `deferred_ticks` grew every second while
  `checked` stayed at 0.
- Render callback: atomic snapshot load + `drawLines` only; box scratch is a
  pre-reserved `thread_local` vector.
- F3 screen: the housekeeping thread publishes a `F3Snapshot` at 4 Hz
  (layout/formatting in `debug_layout.cpp`, host-tested); the Present thread
  only formats two columns and fills ImGui draw commands.
- Voxel cache: `unordered_map` bounded at 49 152 entries, 20-tick TTL, cleared
  on world change and on size overflow.

## Verification checklist (BOL)

Run Diet alone (no other Mods) and read
`~/Documents/PEXData/Diet/Diet.log`.

1. **Engine alive.** `cull summary: tracked>0` while standing in a populated
   area; `population` matches the visible actor count roughly.
2. **Occlusion decisions.** Enable `cull_debug_draw`. Walk behind a stone wall:
   the boxes of mobs/items behind it turn orange (hidden verdict);
   step back into line of sight and they disappear from the overlay on the same
   or the next tick (immediate show).
3. **Budget respected.** `engine_us` stays below `tick_budget_ms * 1000`
   (allow one sample of overshoot); `budget_stops` may be > 0 in dense areas —
   that is the budget working, not an error.
4. **Frame guard.** On a known-stalling server, `deferred_ticks` rises during
   the stall while `block_queries` stays flat for those ticks.
5. **Pillar 2 localisation.** Reproduce a burst (fast travel / join). In the 5 s
   window containing the spike, `class=chunk` or `class=block-update` with a
   large `burst50ms` and `bytes` ⇒ decode/ingest cost. A spike window with
   `packets=0` ⇒ the stall is not packet-driven (meshing/render).
   `frame_ms p95/max` before/after is the reproducible comparison.
6. **No new stall.** Compare `frame_ms p50/p95` with Diet enabled vs disabled in
   the same spot; the difference is Diet's own cost plus the SDK payload copy.
7. **F3 screen.** Press F3 in a world: the Java-style two-column overlay
   appears (version, fps, XYZ/Block/Chunk, Facing; Wine/CPU/Display/GPU/Mem/
   Allocated on the right; Diet's own `Diet:` lines at the bottom left). It is
   not drawn on the title/menu screens, and F3 again hides it. See
   `docs/diet-f3.md` for the element-by-element mapping.

## Known limits

- Players are never hidden by default (`cull_players` off).
- Block entities (beacons, chests, signs) are not enumerated by the SDK and are
  therefore not culled; the "renders far beyond its bounds" whitelist has
  nothing to attach to yet.
- The voxel cache can serve a stale "opaque" for up to 20 ticks (~1 s) after a
  block is broken; hysteresis makes this a bounded, self-correcting error.
- `skipped=` counts per-candidate callback exclusions, not GPU draw calls.
  The filter does not change terrain submission, actor simulation, or AABBs.
- Packet deferral intercepts exactly five profiled entries. Everything else,
  including every `unknown` id, keeps the untouched path; `SubChunkPacket`
  (174) is not intercepted (its shared_ptr entry is not profiled yet).
- Packet classes cover the ids in the classifier table; everything else is
  `unknown` and therefore Critical, so nothing unknown can ever be deferred.
  In single player the observer also sees serverbound ids (e.g. 144
  `player_auth_input`) because the integrated server decodes through the same
  `Packet::read`.
- The F3 ping line appears only when a real NetworkStackLatencyPacket (id 115)
  round trip is observed. Single-player sessions send no id-115 traffic, so the
  line stays absent there; on a dedicated server only the server's probes are
  decoded locally and the pair rule needs the outbound side
  (`sdk::PacketWire`) to complete (evidence-09 §1).
- The old RenderDragon extraction predicate is still diagnostics-only: its
  frame records have no actor identity. Live exclusion is on the earlier
  `queueRenderEntities` path instead.
- Two marker files in the Diet data directory are diagnostic switches for the
  BOL harness (the OreUI settings store is unavailable before the game UI
  exists): `Diet.probe` selects logging-only for render and packet handlers;
  `Diet.debugdraw` enables the diagnostic boxes. Neither changes a shipped
  default.
