# SpeedBuilder native candidates / 1.26.52.3

Target SHA-256: `4a92bfa3ce2428b40ee517b7c1125d9f6f79274382de03846351992663b2e2e4`.
No cross-version addresses or borrowed old-client offsets are enabled without
exact-build attachment. All new bindings remain candidate / runtime_verified=false.

| Function | RVA | Windows x64 ABI / evidence |
|---|---|---|
| ClientInstance region | 0x5DAEBB0 | RCX ClientInstance, returns native BlockSource pointer. `ev_af167bba2797b60ffe45d17af648fa2edcc644be90a65a64b07bf3d2d2959fff` |
| BlockSource getBlock | 0x2D80120 | RCX source, RDX BlockPos pointer; returns interned Block pointer. `ev_f00a2d8382b7c94b9a4906c20fc8af347d5f96c6927b13b4385e5ca15cc469d5` |
| PlayerInventory selectSlot | 0x1F4860 | RCX supplies, EDX slot0..8, R8B Inventory ContainerID0; returns AL boolean, follows selection notification. `ev_e7280da801fec58140c2eb36b18894b34cf02a2c2f8290891ae57279b5f5529d` |
| GameMode buildBlock | 0x259BC60 | RCX GameMode, RDX support BlockPos, R8B face, R9B hand0, stack+0x28 bool isSimTick=false; returns AL. `ev_07f9860a4edc80ba3885f69bfe84649d59d1916b638291a4920a35e31edc4fab` |
| BlockType placement state | 0x3184DC0 | RCX type, RDX actor, R8 target, R9B face, stack Vec3 click pointer and int aux. Resolves aux/default state, invokes const placement callbacks. `ev_8bfbdd8e95a06043f5c9b662c3c7cf0b36c98b3a0847d2fc885b060393e96eaa` |
| Level getHitResult | 0x117D580 | `mov rax,[rcx+0x1e8]; ret`; native Level virtual slot0x14F |
| SetTitlePacket stream read | 0x3A83770 | RCX Packet, RDX hidden Result<void> output, R8 stream. Success flag at Result+0x40. `ev_05c5c1018ab7d235651741c3bc7c4c5c623fc607ea3692b9f1ebb0ac504b619b` |

SetTitlePacket vtable `0xE89D5E0`, type+0x30, MSVC string+0x38 is supported by
constructor `ev_b599387268910ce4ece43d8e0cc024e866c013c8cd1d966b39f79d34376cb444`.
The post-read hook only copies bounded successful title payloads into a bounded
mailbox. It never cancels traffic, retains Packet pointers, or touches world data.
Actual live receive routing is unverified. Rawtext literal `text` nodes are supported;
translation-key resolution is not implemented.

The native transaction closure at `0x25B4E70`
(`ev_9cd48a09599490c10dd724724053b2f8351e790ea6334881e2540241be774c0c`)
reads Player+0x5B8 -> selected slot+0x10, Player+0x1D8 -> Level, and Level
virtual+0xA78 -> HitResult. HitResult type+0x18, face+0x1C, support BlockPos+0x20,
and hit Vec3+0x2C are copied into the native transaction. Absolute hit minus
support coordinates becomes the fractional transaction click. The implementation
temporarily replaces and restores only the first0x38 scalar bytes; it never
copies or overwrites the weak entity references after that prefix. The inventory
transaction and packet object construction remain native.

A detail that must not be ported incorrectly: the GameMode virtual slot13
useItemOn call uses a hidden InteractionResult output pointer in RDX. It is **not**
a plain boolean-return function with ItemStack in RDX. SpeedBuilder avoids calling
that ABI directly; it calls the native build wrapper and its normal transaction.

Native GameMode vtables `0xE827560` / `0xE827600`, slot6. The latter's slot6
`0x25A0810` tail-forwards to the base wrapper after a trial check. Player+0xAA0
is corroborated by native wrapper `0x268CD90`; validate vtable and GameMode+8
owner identity before use. BlockSource's slot2 is checked against getBlock, and
slot41 must return a live chunk before a getBlock call (the native fallback
otherwise returns an air block for missing chunks).

Block type+0x68, full-name MSVC string+0xE8, flags+0x164/solid bit1,
ItemStack block+0x18 and aux+0x20 are layout candidates corroborated by the
existing SDK and current renderer/source declarations. Different block objects
at different positions share interned states; their numeric identities are
compared only within the same world generation. No worker dereferences them.

Primary references consulted for ABI/layout meaning:
- LeviLamina GameMode, PlayerInventory, BlockType and HitResult declarations.
- Latite BlockSource / Block / ItemStackBase declarations.
- Solstice 26.51 getDestroyRate pattern as a localization hint. A partial pattern
  match alone was rejected where Ghidra showed unrelated telemetry code.

Evidence slices and full exported REA bundles are outside the repository:
`~/Documents/PEXData/REA/speedbuilder-title-1.26.52.3/` and
`~/Documents/PEXData/REA/speedbuilder-native-1.26.52.3/`.
Static evidence establishes candidate call shapes, not successful runtime placement,
server acceptance, all block orientations, or immunity to native assertions.

## Live placement follow-up, native revision 2

User logs establish that title capture, the Japanese prompt, volume capture and
stable removal diff execute. They do not establish successful placement. The
previous `placement unresolved` line omitted every reason for rejecting poses.
The new adapter preserves exact-state matching and reports support/reach/native
prediction failure/state mismatch counts, expected/predicted Block data, effective
hotbar metadata, dispatch attempts/returns and pre-dispatch deferral reasons.

`ItemStackBase` method RVA0x248D280 reads raw aux+0x20, uses Block+0x120 when the
Block exists and raw aux !=0x7FFF; the sentinel is preserved. REA/Ghidra evidence
`ev_8056d6a2d217bc31b9c0bff01ce9e370d409dd6aea7107ae82af82c97261d46b`,
`item-data-review-evidence.json`. This establishes native metadata selection;
it does not prove every block placement callback's meaning. Prediction now runs
with the actual hotbar item selected, not the user's previous carried item.
The selection lease spans bounded probe steps, restores only if still owned and
in the same World/RuntimeID, and transfers to the pending placement on success.
Rotation is applied immediately before dispatch and restored only if still owned,
so normal input updating orientation between ticks cannot silently defer forever.

For non-solid support candidates only, BlockType collision-shape virtual slot4
is called using the renderer's exact hidden-return ABI: RCX type, RDX AABB output,
R8 Block, R9 IConstBlockSource, stack BlockPos pointer and null optional entity.
Existing cube-render evidence at0x47B0F4E..0x47B0F7F establishes register placement;
LeviLamina's AABB-returning declaration corroborates it. Only finite, positive,
bounded AABB extents qualify. The full preview capture does not run collision
queries. No guessed `canBeBuiltOver` vtable call is enabled: inspection of slot83
showed a component lookup rather than the expected four-argument bool getter.
Saved rejection evidence: `rejected-support-slot83.snapshot.json`.
These native bindings remain candidates pending the next live logs.

End-of-round Japanese subtitles cancel capture/wait/build work. Pure UTF-8 section
sign sequences are removed as formatting; lone0xA7 continuation bytes are preserved.
Unchanged semantic configuration no longer causes a needless cancellation.

## Named properties and post-placement permutations, revision 3

The new live log proves four native sandstone placements were accepted and all four
exact states observed. It also isolates wall/hive predictions as same-type, differing
pre-placement state (e.g. wall41 vs1). Revision3 permits same-native-type proposals
and validates completion only against the actual recorded state. Walls/connection
states can settle after adjacent placements; real same-type supports are usable
before their final permutation. Candles/cluster_count allow bounded incremental use
only when the observed count is below the goal, never blind repeated clicks.
Provisional type observations are tracked separately from exact confirmations.

A read-only NBT view copies named properties once per interned Block state.
REA/Ghidra ev_7384df9c2cbd7961b2b68138bb1d131fbff5b3a9923acc5d5d609104aecf07ce
at0x2548830 establishes tree head+8, root parent+8, nil byte+0x19, key+0x20,
TagVariant+0x40 and discriminator at node+0x68 (variant+0x28). Block+0xF0 is a
candidate inline CompoundTag layout corroborated by declaration sizes and leaf
LEA+0xF0 candidates. The runtime reader checks Compound getId leaf bytes (without
calling it), bounded acyclic parent-consistent trees <=128 entries, matching root
name and compound states. Only byte/int/string properties are exposed; any mismatch
returns unavailable. It never calls foreign STL methods or mutates NBT. Immutable
SDK-owned property copies, not native pointers, may cross to the diff worker.
Evidence: PEXData/REA/speedbuilder-states-1.26.52.3/compound-map-evidence.json.

Standard cardinal/facing names guide candidate yaw/pitch. Arbitrary custom bit
values are not interpreted as orientation. Exact predicted candidates are preferred;
otherwise known non-derived properties are preferred, then the best same-type
proposal across matching hotbar slots is selected and rechecked before dispatch.
The actual resulting permutation still must match. Unresolved server-defined
orientation can remain provisional and is logged; success is not inferred from
native acceptance. Microsoft primary block-state listings document wall connections
and candle counts: https://github.com/MicrosoftDocs/minecraft-creator/blob/main/creator/Reference/Content/BlockReference/Examples/IntrinsicBlockStatesList.md .


## Revision5 — sparse observations and orientation lead

The user's radius20 recording contains68,921 cells per full pass,256 cells/tick and a second-removal-pass wait that lasts to round end. Native calls were not the stopping point in that round. Another round reached33 accepted native placements but retained five type-only states. The user confirms those are orientation errors, not wrong target coordinates.

The initial scan now enumerates unique cube shells in O(volume), recording the center before far corners without a game-thread sort. Freeze that first full baseline. Later observations contain only original non-air positions; worker validation checks completeness against exactly that non-air set. Original air positions cannot become placement targets. The baseline is copied once to an immutable shared snapshot, and sparse after-snapshots are copied per completed observation pass. Main-thread sampling keeps the cooperative1000us deadline and increases only the old default row cap256→1024. A slow individual native read can exceed the cooperative budget; it is not a hard deadline.

Worker computes the first removed candidate list; the owning client tick rereads those positions on a later tick, in batches capped at128 with a cooperative750us deadline. Any non-air reappearance rejects that candidate batch and resumes observation. Unknown reads wait without issuing placements. No second full-volume pass is required. A fresh title/world token invalidates caches and in-flight checks.

Native pose selection now precedes the build call by two client ticks, with the planned angle re-applied while waiting. Prediction still restores its temporary changes; pending placement retains an owned angle lease until dispatch/cancel. This gives the normal input pipeline a chance to serialize the planned pose. It does not prove the server used that pose or establish the meaning of arbitrary custom direction integers. No additional packet layout, native RVA, block deletion or state write is introduced.

Actual custom-state mismatches are logged with goal/current properties and absolute target coordinates. The exact support coordinate, face and yaw/pitch used are logged for each dispatch. Type-only matches remain provisional. Standard and Hive-specific server results require live validation; do not label the custom yaw mapping verified.
