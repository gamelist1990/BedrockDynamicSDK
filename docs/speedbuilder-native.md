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
