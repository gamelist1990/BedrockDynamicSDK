# Diet

A lightweight native Mod for BedrockDynamicSDK that targets client load and
network-induced stalls. Target game version: **1.26.65.1** only. Output:
`Diet.dll` (standalone DLL, loaded like HitBox/DynamicClient).

## What it does today

1. **Occlusion decision engine (Pillar 1).** Enumerates actors through
   `bedrock::sdk::ActorFacade` (EnTT registry walk), plans AABB sample points,
   and runs voxel DDA raycasts against `bedrock::sdk::WorldAccess`
   (`BlockSource::getBlock`). All decisions run on the game tick inside a
   configurable microsecond budget, staggered per target by distance, with
   hysteresis (hide after N consecutive fully-blocked checks, show immediately
   on any visible sample) and a distance stage that culls without raycasts.
   The resulting verdicts are published as an immutable per-tick snapshot that
   the render callback only reads (no locks, no allocation).
2. **Frame/packet correlation (Pillar 2, measurement).** `sdk::PacketObserver`
   (post-decode, networking thread) feeds a lock-free ring; the render callback
   feeds the frame clock. Every 5 s `Diet.log` reports frame p50/p95/max, spike
   count, and per packet class: packets, bytes and densest 50 ms burst.
3. **Frame-time guard.** When the previous frame exceeded `net_budget_ms`,
   Diet performs no work at all on that tick and counts the deferral.

## What it deliberately does **not** do

The SDK has no confirmed surface for either of these, and this build does not
guess at one (see `docs/diet.md` and `opencode-report.md`):

- **Skipping an actor's draw call.** There is no per-entity render hook. HitBox's
  precedent is post-render *drawing*, not skipping.
- **Deferring game packets.** The receive hook fires after `Packet::read`, and
  the SDK forbids retaining packet payloads; the inbound queue is not exposed.

Both need new exact-build RVAs **and** BOL runtime verification, which this task
was not allowed to perform. The culling verdicts are published and can be
visualised (box overlay), so the engine can be validated before the skip
surface exists.

## Settings (Mod Settings, persisted)

| id | default | meaning |
| --- | --- | --- |
| `diet_enabled` | on | master switch |
| `cull_enabled` | on | occlusion/distance culling engine |
| `cull_players` | off | never hide players when off (recommended) |
| `cull_leaves` | off | treat leaves as occluding blocks |
| `cull_max_distance` | 96 | beyond this: distance cull without raycasts |
| `tick_budget_ms` | 0.7 | hard per-tick budget for culling work |
| `cull_debug_draw` | off | draw hidden targets as orange boxes |
| `net_measure` | on | frame/packet correlation reporting |
| `net_budget_ms` | 3.0 | skip Diet work on frames slower than this |
| `debug` | off | verbose logging |

## Log

`%USERPROFILE%/Documents/PEXData/Diet/Diet.log`
(BOL: `~/Documents/PEXData/Diet/Diet.log`).

## Build

`./build-linux.sh` (adds `Diet` to `CLIENT_MODS`), or directly:

```sh
cmake -S exsample/Diet -B build-linux-Diet -G Ninja \
  --toolchain "$PWD/linux-xwin-toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
  -DFETCHCONTENT_SOURCE_DIR_BEDROCK_SDK="$PWD" -DBEDROCK_GAME_VERSION=1.26.65.1
cmake --build build-linux-Diet -j
```

Host tests (no Windows toolchain needed):

```sh
cmake -S exsample/Diet -B build-host-Diet -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-host-Diet && ctest --test-dir build-host-Diet --output-on-failure
```
