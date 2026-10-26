# Top native candidates / 1.26.52.3

Exact executable SHA256: `4a92bfa3ce2428b40ee517b7c1125d9f6f79274382de03846351992663b2e2e4`.
All new bindings are `candidate / runtime_verified=false`.

Full-PE REA/Ghidra import exceeded its330000ms startup deadline. Function bytes
were then extracted from this PE's original `.pdata` ranges and examined with
REA/Ghidra raw-binary mapping at the original VA. Discovery uses literal bytes,
recognized RIP-relative LEA references and exact vtable pointer values. These are
static observations, not live validation. Raw slices use Ghidra's default GCC
compiler spec, so pseudocode parameter numbers are not Windows ABI proof; x64
register/stack instructions and callers are the ABI evidence. Unmapped CFG
trampolines/imports and decompiler terminal-call assumptions limit pseudocode.

- CommandRequestPacket table `0xE897420`: slot1 getId `0x38E6CB0` returns77;
  slot2 getName `0x38E6CC0` returns the CommandRequestPacket literal.
- Writer `0x38E6D00`: RCX Packet retained as RDI, payload address `RDI+0x30`.
  The source declaration of CommandRequestPacketPayload starts with MSVC string.
  Evidence `ev_4097a24c921f54e8a8fcb9e61281a0c41e9fbcd465af646f8e7f2760c404f7f5`.
  Only table-matched packets are read, with a2048-byte bounded SSO-aware copy.
- ClientInstance vtable `0xE9731B0` is matched by existing getLocalPlayer slot31
  `0x5DAED30` and getPacketSender slot293 `0x80B340`. Slot217 is the GuiData getter
  `0x5DC5830`, reading ClientInstance+0x650. It returns a guarded non-owner result
  whose raw object is result+0x10, equal to the pointer read at ClientInstance+0x650.
  Evidence `ev_06cd0676a4f9439790988afdfe48f165bbb2f72b839908eae884b5c532d01f73`.
- GuiData message candidate `0x1560AE0`: callers including `0x878310` supply
  message string, empty optional string, empty TTS string and stack filter context4.
  Registers RCX/RDX/R8/R9 preserve self/message/optional/TTS; native body reads
  TTS+0x10. Cross-reference with LeviLamina displaySystemMessage declaration.
  Evidence `ev_88e76570b9a11e33d34759c9436ebba76b71ad2ee98cc633c41c51331d64cd50`.
  Native entry bytes, the alive flag reached through GuiData+0x30, and
  GuiData+0x98 owner equality are checked. GuiData is polymorphic: its first byte
  is part of a vtable pointer, never an alive flag.
- Existing BlockSource getBlock evidence `ev_f00a2d8382b7c94b9a4906c20fc8af347d5f96c6927b13b4385e5ca15cc469d5`
  compares Y against source+0x3A signed min inclusive and+0x38 signed max exclusive.
- Actor position/AABB/RuntimeID use the existing SDK layout. Previous position,
  velocity and fall-distance layout reuse Freecam Borion fields. Updates happen
  on the owning client tick and attempt rollback if any guarded write fails.

Saved evidence, extraction manifests and bytes:
`/home/pexserver/Documents/PEXData/REA/top-1.26.52.3/`.

LeviLamina getOrCreateCommand uses service::getCommandRegistry(isClient),
registry.registerCommand, then SyncLocal on client registrations. Revision1
used a narrow immutable SDK ClientCommands table and packet interception.
Revision2 adds guarded native registration as described below.
Only registered names are consumed. No packet pointers or ECS objects move to
workers. The bounded mailbox records the world epoch and stale commands expire.

Safety is conservative, not a full Minecraft physics simulation: body cells must
be air; full support requires existing native solid flags and an explicit vanilla full-cube
identifier allowlist. Partial/hazardous/custom and unlisted support types are rejected. Runtime
safety, integrated/multiplayer command routing, server corrections and Japanese
chat display require live validation. A build success establishes none of these.

## Revision2: native command registration and completion

Exact-build evidence is retained under
`/home/pexserver/Documents/PEXData/REA/top-completion-1.26.52.3/`.
Discovery is literal builtin description strings -> recognized RIP-relative LEA
references -> original `.pdata` ranges -> REA/Ghidra. The memory-mapped extractor
avoids whole-image duplicate byte buffers. Full-image analysis is not repeated.

| Entry | RVA | Evidence |
|---|---|---|
| Help setup | 0x80E7250 | `ev_71b7492a78659b9e2c0fcd6831d5edece2252b5e098cd73df86110fefebf1c48` |
| registerCommand | 0x4A78840 | `ev_72b721b2d99f368a86f80c6217c57fe5a34a12eb3a83d540d0d06f19b866ca1e` |
| findCommand | 0x4A56630 | `ev_131f6e121ee6efabddbca73c5a98b58a9e252ea991f3a97bb19a7914d0a3bab5` |
| Parameter constructor | 0x4A47C80 | `ev_021470f8834c2fd4b4e816cc8332790e256566d80dabe48b2a68a9855c145b98` |
| One-parameter overload helper | 0x4A224D0 | `ev_768d5883b24cf069aac46a82804b4311d47ed3702538485b71e3604e4110e744` |
| Client initCommands | 0x5DB9F00 | `ev_fc0c0ccbd2548e064fedb0c1b353b63b7139c76603ba192004677516b3613ba3` |

The builtin Help call supplies `Any=0`, flags `0x10 / 0x80`, corroborating the
LeviLamina SyncLocal / NotCheat declaration. Its page:int constructor supplies the
16-bit type-id global at0x11E141D0 and ParamParseRule at0x11E145F8. These are runtime
BSS objects, not static file values; the adapter waits until the rule points to
executable code. Parameter size0x78/name+0x18/offset+0x58/setOffset+0x5C/optional+0x60
are observed in the constructor. The adapter uses int+0x20, hasValue+0x24 in a
0x28-byte Command instance. Help factory0x811DAC0 corroborates a0x20-byte Command
base and vtable0xEA0D0C0 with destructor / collectOptionalArguments / execute.
The default collectOptionalArguments leaf0x848A0 returns true.

The overload helper accepts registry RCX, name RDX, packed CommandVersion R8,
callable R9 and one ParameterData pointer on the stack. It looks up the signature,
appends a0x80-byte overload, deep-copies one0x78-byte parameter, then invokes
registerOverloadInternal0x4A7C6A0. Signature overload vector begins at+0x40.
All registry/vector/string allocation and copy construction happen in native code.
The temporary name `blocks` remains short-string storage, so the temporary owns no
native heap allocation that would need a foreign CRT destructor.

The native64-byte small callable has vtable0xE8EEE40 and free-function pointer+8.
Its four slots are copy0x791D0 / destroy0x76BC0 / invoke0x9B7880 / copy0x791D0.
The copy thunk copies only+8; destruction is a no-op; invoke takes hidden unique_ptr
output RCX and callable RDX and calls the function at+8 with RCX unchanged. The
adapter supplies a pinned DLL factory with the matching hidden-return ABI. Its
Command's virtual deleting destructor runs in that same DLL. It never inserts
DLL-owned std::vector buffers or heap strings into game-owned containers.

Client+0x1B0 -> Minecraft+0xC0 -> MinecraftCommands+0x10 is the registry path. The
initCommands body also writes MinecraftCommands+0x18 as the output sender, matching
the LeviLamina class declaration. The hasCommands leaf0x5DC2120 checks the same
Minecraft+0xC0 pointer. Help execute0x80E7740 increments CommandOutput+0x28 on
success; the callback marks an accepted queued request at that offset.

Registration and checks run on the client tick, at most once per20ticks. Existing
same-name foreign metadata is preserved. The factory is confirmed in the first
native overload before treating an existing signature as ours. World changes
invalidate the current registry lease; callbacks from an expired registry are
ignored. Native and packet fallback requests are paired by canonical command,
world epoch and opposite source within200ms to prevent double dispatch. Deliberate
repeated commands from the same source are preserved. Top_Stop stops execution;
DLL unload and live replacement are unsupported because native metadata retains
factory callbacks until the registry is destroyed.

These remain static candidates, not ABI/runtime-verified SDK symbols. Runtime
completion, optional-argument parsing, local execution, output bookkeeping and
re-registration after server metadata updates still require a game observation.
