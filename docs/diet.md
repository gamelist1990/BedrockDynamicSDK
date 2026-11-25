# Diet — surfaces, gaps and verification

Target: 1.26.65.1 (`data/1.26.65.1/windows-x64/manifest.json`, exe sha256
`dacd67e7…093b88`). Evidence for each claim lives outside the repo in
`~/Documents/PEXData/diet/evidence-01-surface.md` … `evidence-03-build.md`.

## Surfaces Diet builds on (all confirmed)

| Need | Surface | Notes |
| --- | --- | --- |
| block read at (x,y,z) | `sdk::WorldAccess::getBlock` → `BlockSource::getBlock` (`0x32CE180`, verified) | returns `Cell{identifier, kind, properties, supportsPlacement}`; solidity is the native `BlockType.flags & 2` read at `+0x164` |
| actor enumeration + AABB | `sdk::ActorFacade::capture/bounds/position` (EnTT storage intersection) | value copies only; runtimeId identity |
| local player / eye position | `ActorFacade::localPlayer` + `WorldAccess::pose` | camera = position + 1.62 |
| tick | `sdk::Hooks::onLevelTick` | native reads stay on the game thread |
| render frame clock + overlay | `sdk::WorldRender::onPostRender` | read-only snapshot consumption |
| incoming packet observation | `sdk::PacketObserver` (new, wraps `PacketAPI::onAnyReceive`) | post-decode, networking thread, payload size only |
| settings | `sdk::Mod` + `sdk::SettingsPage` | persisted by the Mod Settings store |

## Surfaces Diet needs and does not have

| Need | Missing surface | Why it blocks |
| --- | --- | --- |
| skip a hidden actor's draw | per-actor render dispatcher entry, or a custom `LevelCullerBase` injected into the renderer | `BDS_HOOK_RENDER_LEVEL` is per frame; `LevelRendererCamera::render` (`sub_5376690`, `0x5376B10`) is per camera. HitBox's precedent is post-render drawing. `queueRenderEntities` exists as a string in the image but its RVA/data path is unverified (`docs/view-actor-enumeration-investigation.md:72-92`). |
| defer delay-tolerant packets | inbound queue / handler dispatch entry | the receive hook runs after `Packet::read` and must call `next()`; payloads are borrowed; the SDK forbids retaining packet objects. |
| smooth remote entities / blend local corrections | a per-actor render transform hook, or a correction-handling entry | writes to native position components from Mod code are a mutation the brief forbids; no correction path is profiled (`MovePlayerPacket::isValid` only validates). |
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

## Budget and threading

- Actor capture: `ActorFacade::capture` incremental cursor (96 rows/tick) with
  the facade's own cooperative budget.
- Raycasts: `OcclusionEngine` stops before starting any check once
  `tickBudgetUs` (default 700 µs) is exhausted; a partially completed target is
  left unchanged (never hidden on partial evidence).
- Per-target schedule: 120/300/600 ms by distance band; no target is evaluated
  every frame.
- Frame guard: a frame slower than `net_budget_ms` defers **all** Diet work.
- Render callback: atomic snapshot load + `drawLines` only; box scratch is a
  pre-reserved `thread_local` vector.
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

## Known limits

- Players are never hidden by default (`cull_players` off).
- Block entities (beacons, chests, signs) are not enumerated by the SDK and are
  therefore not culled; the "renders far beyond its bounds" whitelist has
  nothing to attach to yet.
- The voxel cache can serve a stale "opaque" for up to 20 ticks (~1 s) after a
  block is broken; hysteresis makes this a bounded, self-correcting error.
