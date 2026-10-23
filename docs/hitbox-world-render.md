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
- Five-tick sweep target: ceil(population/5) rows per tick, maximum 32, with the
  existing cooperative 250us loop budget. Slower sweeps continue instead of
  restarting; the upper bound and budget can extend a sweep beyond five ticks.
- Linear interpolation uses copied old/new bounds each render frame. New samples
  start at the current interpolated position. Teleports beyond eight blocks,
  sample gaps over two seconds, and world epochs reset interpolation. No motion
  extrapolation is performed; smooth display adds up to a sample interval of lag.
- Native actor capture stays on the game tick; two workers receive copied
  RuntimeID/AABB/position data. Native drawing stays inside the world render pass.
- Twelve edges per AABB, at most two color-group mesh submissions, at most 512 boxes per pass. The
  selected material determines depth behavior and line width. The legacy
  `line_thickness` setting does not change native line width.
- Active native tessellation is skipped. Read/write guards and SEH reduce the
  risk of invalid accesses but cannot make an incorrect native ABI or corrupted
  engine state safe. A guarded draw fault disables further HitBox drawing.
- Local tests validate coordinate conversion, interpolation/teleport resets, five-tick scheduling, retention/world epochs, and profile
  rejection. Cross-compilation/Windows CI establish compilation, not in-game
  visuals, third-person correctness, or a measured FPS improvement. Those remain
  runtime acceptance checks on this exact build.
