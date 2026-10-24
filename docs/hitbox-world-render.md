# HitBox native world rendering: 1.26.52.3

## Scope and confidence

The implementation follows Latite's post-`LevelRenderer::renderLevel` drawing
path, using the game's ScreenContext/Tessellator and a name_tag material obtained through the native material constructor. Reference source: LatiteClient/Latite commit
`9f7463515dd298a496da918285936d78c7416aad`,
`src/client/memory/hook/hooks/LevelRendererHooks.cpp`,
`src/util/DrawUtil3D.cpp`, and the native renderer declarations.

Exact original Windows x64 executable SHA-256:
`4a92bfa3ce2428b40ee517b7c1125d9f6f79274382de03846351992663b2e2e4`.
These are static observations and ABI inferences, **not runtime verification**.
`native_world_render` is kept separately from the SDK's verified symbol table,
with `status=candidate` and `runtime_verified=false`.

REA/Ghidra could not complete whole-image analysis within either the 330-second
or the scoped 1200-second startup limit. Focused analysis used unchanged function
bytes extracted from the original PE, loaded at their original virtual addresses.
Function boundaries came from the PE exception directory where available. The
raw slices do not contain external callees, global data, or incoming xrefs. Their
default decompiler convention is not authoritative for Windows: ABI deductions
below use actual register instructions plus Latite's Windows declarations.

Evidence bundles, slice hashes, and original file offsets are retained outside
source control in `Documents/PEXData/REA/world-render-1.26.52.3/`.

## Observed path

| Function | RVA | Win64 call shape | REA Evidence ID |
|---|---|---|---|
| LevelRenderer::renderLevel | `0x047f8570` | RCX renderer, RDX ScreenContext, R8 context; source declaration returns void | `ev_4fc07be63ed7f16412e9e9e6cd2fab39a3fa79b433ba94ecc2c291f1df712a88` |
| Tessellator::begin | `0x059ca2e0` | RCX tess, EDX 0, R8D primitive, R9D vertex count, stack bool | `ev_242f9ff6cc167eae6c175516d20b56511b41b6fda8bda93cb671647ff2b13450` |
| Tessellator::vertex | `0x059ca7a0` | RCX tess, XMM1/XMM2/XMM3 xyz | `ev_41f68164f3d6d7b161133bdb15ee6ecf0f9ada6b5c6a7c847f29f4d47829c61c` |
| Tessellator::color | `0x059ca610` | RCX tess, RDX pointer to float RGBA | `ev_6e5b4db260733eb9667df675d0ecc3e63e4f89357e7196f142be1204356ff9bf` |
| MeshHelpers::renderMeshImmediately | `0x0467ee50` | RCX screen, RDX tess, R8 MaterialPtr address, R9 zeroed 0x58-byte scratch | `ev_01ab8da87735a4caaebc275f302d80763e03272f98861c2fac665f9944df38b1` |

The native selection drawing routine at `0x046b6000` loads the tessellator from
ScreenContext+0xb8, shader-color pointer from +0x30, and the render origin from
LevelRendererPlayer+0x660. It passes the inline material at player+0x10f0 to the
mesh helper. It also writes shader RGBA and its dirty byte at +0x10.
Evidence: `ev_8f5cfe23d20735089ae6edffe9491da226338d2a1dcfc3166ffc92c60501542b`.

The constructor at `0x04697190` constructs the inline string `selection_box` at
instructions `0x04697de1..0x04697df9`, takes this+0x10f0 at 0x04697dfd, then calls
the material constructor with that address in RCX at 0x04697e20. This corroborates
the material's identity rather than guessing it from an offset.
Evidence: `ev_c007da2031b46b3fc710c04ec98ff0a68ef50c0949bfa46b1324eef9ad3fb5ec`.

The renderLevel body accesses LevelRenderer+0x468 for the player renderer. Begin
checks tess+0x2a0 and +0x255 before starting. Vertex adds tess translation +0x1cc;
HitBox supplies origin-relative vertices with that translation temporarily zeroed,
then restores the previous translation and shader color. The screen, tessellator and renderer are borrowed from the engine.

The initial selection_overlay attempt followed Latite, but the user reported
that walls still occluded the boxes and their colors appeared faint. The current
implementation uses `name_tag` instead (material-source evidence below). The common material group is addressed at RVA `0x11e3b308`, the same
RDX address loaded by the original constructor at 0x04697e12. A separate native
caller at 0x0442c7c8 passes a char literal to HashedString constructor 0x006ac970,
then passes its output and the common group to MaterialPtr constructor 0x052f1e00.
The two focused REA dossiers establish RCX output / RDX const char* for the first
and RCX output / RDX group / R8 HashedString* for the second. The constructors
write 48 and 16 bytes respectively. The actual hash algorithm is computed by the
engine; it is not reimplemented from the historical reference.

- HashedString ctor: `ev_4dd77c2d31b4b68ed41aa9dc578a89cce4b481af545b88fcfff0406c5ac37581`
- MaterialPtr ctor: `ev_c13e00454c6069e2341abdca35ca45acf1c8a13e3fdfb76aafed9317a3142d2f`
- Native construction caller: `ev_7b893d086817d8b02c881408a1afb404e85944901183c393c2a4eff1576c75db`

Both constructors are called once on the render thread after the common group's
factory is available. Native internal allocations and reference counts are held
until process exit, matching the pinned DLL lifetime. This bounded lifetime avoids
calling a foreign STL's destructors. Initialization failure disables HitBox drawing.
### Correction from the shipped material sources

The locally installed exact-version assets contain
`data/resource_packs/vanilla/materials/ui3D.material` (SHA-256
`61a9a6589bf2e9cdacb76adae26b0a0d7f93f77d203f7bbf668e67c6845ee7eb`). `common.json` loads that material file.
The `selection_overlay:block_overlay` definition inherits depthFunc=LessEqual,
StencilWrite/EnableStencilTest, and UV1/UV0 fields; its blendSrc=DestColor and
blendDst=SourceColor combine the result with the background. It is not an
always-visible untextured line material on this build. The prior inference from
Latite's transparent option was insufficient.

The independent `name_tag` definition uses depthFunc=Always, DisableDepthWrite,
Position-only vertices, shaders/position.vertex, shaders/current_color.fragment,
and supports both MSAA/non-MSAA. It does not inherit the stencil/UV settings.
HitBox now supplies opaque shader RGBA, grouping live/retained boxes into at most
two mesh flushes because this shader reads the uniform rather than vertex colors.
Old configuration alpha is ignored for the boxes. These asset observations and
hashes are preserved in `material-state-findings.json` beside the REA evidence.
Actual in-game depth/opacity behavior still requires confirmation; the compiled
RenderDragon material was not decoded in this investigation.

## Implementation boundaries

- Only the exact attached executable profile is accepted. All seven function
  entry guards (24 bytes) must still match loaded memory before installing.
- One-tick sweep target: population rows per tick, maximum 32, with the
  existing cooperative 250us loop budget. Slower sweeps continue instead of
  restarting; the upper bound and budget can extend a sweep beyond one tick.
- Linear interpolation uses copied old/new bounds each render frame. New samples
  start at the current interpolated position. Teleports beyond eight blocks,
  sample gaps over two seconds, and world epochs reset interpolation. No motion
  extrapolation is performed; smooth display adds up to a sample interval of lag.
- Native actor capture stays on the game tick; two workers receive copied
  RuntimeID/AABB/position data. Native drawing stays inside the world render pass.
- Twelve edges per AABB, color-group mesh submissions of at most 512 boxes each, without a total 512-box cutoff. The
  selected material determines depth behavior and line width. The legacy
  `line_thickness` setting does not change native line width.
- Active native tessellation is skipped. Read/write guards and SEH reduce the
  risk of invalid accesses but cannot make an incorrect native ABI or corrupted
  engine state safe. A guarded draw fault disables further HitBox drawing.
- Local tests validate coordinate conversion, interpolation/teleport resets, one-tick scheduling, retention/world epochs, and profile
  rejection. Cross-compilation/Windows CI establish compilation, not in-game
  visuals, third-person correctness, or a measured FPS improvement. Those remain
  runtime acceptance checks on this exact build.

## Complete Actor coverage and optional Player filter

`show_all_actors=true` removes distance exclusion; the default now limits it to 100 blocks.
`players_only=false` keeps all valid ActorOwner/RuntimeID/AABB entries, including
entries lacking ActorType. The former ActorType-presence filter and 64-block AABB
extent cutoff were removed; owner/entity generation, finite coordinates and
positive-volume bounds remain required. Native pool size sanity checking remains
16384 entries as a corruption guard, not as the tracking limit. Tracking admission
is a shared 1000-RuntimeID budget across both workers. Live and retained copies
use the same slot. A skewed shard can consume all slots; there is no fixed 500
quota per worker. At capacity, new IDs are rejected while existing observations
continue. Expired/out-of-range live records release a slot only if no saved marker
remains; expired/disabled saved markers release it only if no live record remains.
World epochs reset admission and prevent old-epoch releases from freeing current
slots. Admission locks run on processing workers only. Render snapshot aggregation
also caps the combined frame to 1000 boxes during asynchronous handovers.
With the 32-row limit a 1000-entry pool needs at least 32 callbacks; the one-tick
target does not guarantee every Actor is refreshed each tick.


Rendering splits all snapshot boxes into 512-box mesh submissions for each color,
instead of selecting only the first 512. The two worker inputs are fixed eight-slot
FIFOs. A capture waits in game-thread-owned copied storage until both shards accept
it; capture is postponed under backpressure without blocking the game thread.
Already accepted shards are not sent duplicates. World epochs discard old-world
queued work. These policies preserve captured rows, though sustained overload can
extend the one-tick target and freshness expiry still applies.

`players_only=true` classifies Actors by ActorTypeComponent, not NameTag or display
names. Latite's ActorTypeComponent is one uint32 at offset zero; Actor::isPlayer
compares it with 319. Focused REA/Ghidra analysis of exact-build function RVA
`0x019e9580..0x019e97b6` corroborates this: it looks up component hash `0x4f6ba419`,
uses four-byte component elements and compares `[component_page + index*4]` with
`0x13f` at RVA `0x019e96f6`. The version profile records value_offset=0, value_size=4,
player_type_id=319, retaining candidate status pending live validation.

Evidence: `ev_99841f6e319e9010c0bada1df21a8d6c49d2983fcb5b4b8b4c1eeb4aeea120aa`.
The raw-function slice has the original linked addresses but lacks external
callees and global data; parent EXE/slice hashes and file offsets are in
`actor-type-player-check-provenance.json` outside source control. A first-time component read failure is not treated as a Player. Once confirmed,
Player identity is remembered in a fixed 2048-slot table (at most 1000 IDs) for the
current world. The same RuntimeID retains classification if its ActorType changes
or disappears. Replacement IDs are never linked by proximity or appearance.
Player-only filtering occurs after reading RuntimeID and before reading AABB.

## Retention: latest valid Player observation

With retain_last_position enabled, every valid Player observation overwrites one
saved box for that RuntimeID immediately. There is no stationary delay or
observation-gap requirement. Live interpolation remains independent of retention;
a fresh live box takes precedence over the saved box and avoids historical trails.
When live capture expires, the last measured AABB can be drawn in the retained
color with interpolation disabled. Non-Players and invalid/zero-size bounds never
create a retained box. Delayed samples cannot rewind a newer saved observation.

The saved map shares the 1000-Actor tracking budget with the live cache.
retention_minutes defaults to 10 and is clamped to 1..60. Expiry is measured from
the last valid observation, including for markers outside display range. World
or local registry changes clear identity history and both maps. Disabling
retention clears saved markers. Missing server Actors or a replacement entity
without the same RuntimeID cannot supply a fresh measured position; the saved
box is explicitly a last observation, not a claim of a current live location.

Portable tests cover immediate retention, movement refreshing the saved box,
live/saved deduplication, invalid or zero-size bounds, stale batch rejection,
expiry and refresh, range filtering, bounded Player history, identity reset,
capacity rejection and slot reuse across workers, and render-time freshness.
They establish copied-data behavior, not live Minecraft verification.

## Native build and failure diagnostics

The DLL dereferences game-owned EnTT and C++ container objects, so its C++ ABI
must be compatible with the game. Windows DLL builds now require MSVC/clang-cl
and a release STL layout (_ITERATOR_DEBUG_LEVEL=0); MinGW builds are rejected.
A successful GNU-target cross-compilation and pure-data test are not evidence
of native container compatibility. CI builds MSVC against the SDK checkout from
the same source commit and places that revision in the startup log.

Capture failures now distinguish missing owner/runtime storages, rejected pool
sizes, unreadable local registry context, and guarded native exceptions. The
exception report identifies the attempted operation and exception code/address.
Failed captures retry on later ticks and produce a rate-limited report every ten
seconds; partial batches are preserved and recovery is logged. These details
locate a failed native operation without guessing new memory offsets or silently
turning incompatible reads into successful captures.

The default range is now 100 blocks (show_all_actors=false); nearby_distance
accepts 4..128. Explicit existing configuration is honored, including older
32-block ranges and one-minute retention. show_all_actors=true bypasses distance
filtering. Only loaded Actors with valid captures are available.

## Cached name labels

Name labels use the shared SDK Actor::getNameTag broker to observe returned names;
missing observations are filled on the game thread at most three times per tick within a cooperative 200us budget using
the exact-build resolved getter on a freshly validated Actor. Cached names skip
this fallback; empty results start retrying after 100ms and back off to two seconds per identity. Missing Player names are prioritized. A native fault
disables fallback for the session. Hook callbacks guard a bounded string
copy and a scalar EnTT entity-ID read. A fixed-capacity, try-lock mailbox carries
only copied bytes and numeric comparison keys. The worker joins these against
Actor pointer + entity generation + world epoch from the existing capture, and
stores normalized first-line names by RuntimeID. Empty observations preserve the
previous name; changed generations/worlds cannot borrow an old pointer's label.

Windows DirectWrite lays out the UTF-8 text with system font fallback. Its
GetGlyphRunOutline output is tessellated through a CPU-only Direct2D geometry
factory on the processing worker. See [Microsoft's glyph-run documentation](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwritefontface-getglyphrunoutline).
No game font ABI or new function RVA is introduced. Immutable triangle meshes
are regenerated only on name changes (at most four per worker batch) and retained
with the existing worker-owned snapshots. Rendering converts them to camera-facing
world-space quads through the existing native Tessellator/material path. Each
triangle uses two opposite-winding degenerate Quads to avoid material face culling, following the native primitive numbering used by
the Latite reference; the actual game label draw still needs runtime confirmation.

show_name_tags defaults to true; name_tag_scale is clamped to 0.25..3.0. Labels
share live/retained filtering and epochs with their boxes. CPU names/geometry are
bounded to 1000 RuntimeIDs per worker, and the raw observation mailbox has 2048
slots. Rendering permits at most 98,304 label vertices per frame and batches
submissions; no font shaping, native name read, or text geometry allocation occurs
on the render thread. Windows tests exercise English/Japanese font tessellation;
portable tests cover formatting, UTF-8 boundaries, identity isolation, and billboard
coordinates. They do not establish native game rendering correctness.

### Text-only label rendering correction

Opaque background quads were drawn with depth Always, including for the local
Actor near the camera. They could obscure the scene and native name tags.
Labels now submit glyph faces only and exclude the current local Actor. Each
mesh flush reapplies the requested uniform color and dirty flag immediately
before native submission, including across the 16,384-vertex boundary. Existing
translation/color restoration and diagnostic counters remain active. These
changes address identified code paths; visual correctness still requires an
in-game check.

## Worker-prepared label vertices and render-context epochs

The existing two workers now compute camera-facing name vertices against the
latest copied render origin. Each immutable snapshot owns its own prepared
vertices, label offsets and preparation origin (49,152 vertices per shard).
The render thread sets tessellator translation to preparedOrigin-currentOrigin
and submits the arrays. It performs no per-triangle billboard construction;
engine graphics calls remain in the post-renderLevel callback. The render-camera
mailbox uses try-lock on the render thread; workers read copied scalars only.

Reference review: Latite commit 9f7463515dd298a496da918285936d78c7416aad,
LevelRendererHooks.cpp dispatches after the original renderLevel; PacketHooks.cpp
clears actor name tags on CHANGE_DIMENSION. HitBox additionally invalidates epochs
when renderer/player-renderer identity changes or disappears, and compares
ClientInstance and the locally read RuntimeID. This does not establish packet
handling coverage; unchanged identities during server-side travel remain unknown.

Local-player getter cache lifetime is now 50ms (previously 500ms), so a live
ClientInstance switching its local Actor is checked again within approximately
one normal game tick. Focus/cursor release is not treated as disconnect. The
existing 2s tick-interruption fallback is conservative and may also clear state
when a paused game resumes; it is not claimed as a verified network event.
