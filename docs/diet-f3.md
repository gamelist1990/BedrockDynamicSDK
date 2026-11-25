# Diet — F3 debug screen (Java-Edition layout)

Diet draws an F3 debug overlay through `sdk::Overlay` (the same Present-thread
ImGui surface the other Mods' HUDs use). The screen is deliberately a copy of
the Java Edition debug screen's structure, not a custom HUD.

Reference used for the layout: the documented debug-screen elements and their
line syntax (`minecraft.wiki/w/Debug_screen`, "Information lines" table, read
2026-11-25) plus Java's `Direction.fromYRot` mapping for the facing line.

## Layout rules

* Left column, left-aligned at `x = 2`, one line per element, Java's element
  order: game version, fps, `XYZ`, `Block`, `Chunk`, `Facing`.
* Right column, right-aligned at `displayWidth - 2`, Java's system-block order:
  runtime line, `CPU`, `Display`, `GPU`, `Mem`, `Allocated`.
* Each line has its own translucent grey backing (`0x90505050`, Java's debug
  background colour) sized to the text, white text with a 1 px shadow.
* Diet's own statistics are appended at the bottom of the left column in the
  same style, prefixed `Diet:` so they are visibly part of the screen but not
  mistakable for a vanilla element.

## Element mapping (Bedrock data only)

| Java element | Line syntax implemented | Source | Notes |
| --- | --- | --- | --- |
| `game_version` | `Minecraft <version> (Bedrock)` | selected exact profile | Java prints `(vanilla)`; this client is Bedrock, so that word is the truthful one. |
| `fps` | `<fps> fps` | `TrafficAnalyzer` frame ring (5 s window) | Java appends `T: <max> (<present mode>) @<refresh>`; neither max framerate nor present mode is exposed to a Mod, so they are omitted rather than guessed. |
| `player_position` | `XYZ: %.3f / %.5f / %.3f` | `WorldAccess::pose` (feet) | Same precision as Java. |
| `player_position` | `Block: %d %d %d` | floor of the same position | |
| `player_position` | `Chunk: %d %d %d` | `block >> 4` | Java appends `[cx cz in r.x.z.mca]`; this client has no region-file identity, so the bracket is omitted. |
| `player_position` | `Facing: <dir> (Towards <±X/Z>) (<yaw> / <pitch>)` | `ActorRotationComponent` (pitch/yaw offsets from the profile's actor layout) | Direction from `floor(yaw/90 + 0.5) & 3` = south, west, north, east — Java's `Direction.fromYRot`. |
| `biome` | **omitted** | — | No biome query exists in the SDK's `WorldAccess`; inventing one would be worse than omitting the line. |
| `light_levels` | **omitted** | — | `Cell` exposes the solid flag, not the light values. |
| `local_difficulty` | **omitted** | — | Bedrock difficulty is a world option, not Java's per-chunk local difficulty. |
| `system_specs` | `Wine:/Windows: <version> 64bit` | `ntdll!wine_get_version`, else `RtlGetVersion` | Java's `Java:` line position. The label states the actual runtime. |
| `system_specs` | `CPU: <threads>x <brand>` | `GetSystemInfo` + registry `ProcessorNameString` | |
| `system_specs` | `Display: <w>x<h>` | `GetSystemMetrics` | |
| `system_specs` | `GPU: <adapter>` | DXGI `IDXGIAdapter1::GetDesc1` | Omitted when DXGI cannot report an adapter (e.g. software fallback). |
| `memory` | `Mem: <pct>% <used>/<total>MB` | `GlobalMemoryStatusEx` | Java prints JVM heap; this is physical memory, which is the equivalent this platform has. |
| `memory` | `Allocated: <pct>% <mb>MB` | `GetProcessMemoryInfo` (`PrivateUsage`) | |

Diet's own lines (only when the value was measured):

```
Diet: targets=<n> hidden=<n> skipped=<n> checked=<n> samples=<n> block_queries=<n> budget_stops=<n>
Diet: packets=<n> deferred=<n> queue=<n> classes <name>=<count> ...
Diet: ping=<n>ms
Diet: frame p50=<x>ms p95=<y>ms max=<z>ms
```

`skipped=` appears only when a render-skip surface is active, `deferred=` only
when packet deferral is active, and `ping=` only when a real RTT was measured.
A feature that is not implemented therefore leaves no line — the screen never
reports a zero as if it were a measurement.

## Toggle

* `F3` (key hook) toggles the screen at runtime. The state is session-only.
* The `f3_enabled` setting is the persisted startup state.
* The overlay itself is render-only (`captureInput = false`, no cursor change,
  no ImGui toggle key), so it cannot steal mouse input from the game.

## Known differences from Java

* Font: the SDK overlay's default UI font (Segoe UI) rather than Minecraft's
  font; the layout is column-aligned and monospaced by position, not by glyph
  advance. Diet does not add a font to the shared ImGui atlas.
* No F3+key sub-screens (graphs, profiler, debug options); only the text
  overlay.
* FPS is a 5 s average from Diet's frame ring, not Java's instantaneous
  frame-time average.
