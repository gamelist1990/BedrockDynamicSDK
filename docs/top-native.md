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
  Native entry bytes, GuiData alive flag, and GuiData+0x98 owner equality are checked.
- Existing BlockSource getBlock evidence `ev_f00a2d8382b7c94b9a4906c20fc8af347d5f96c6927b13b4385e5ca15cc469d5`
  compares Y against source+0x3A signed min inclusive and+0x38 signed max exclusive.
- Actor position/AABB/RuntimeID use the existing SDK layout. Previous position,
  velocity and fall-distance layout reuse Freecam Borion fields. Updates happen
  on the owning client tick and attempt rollback if any guarded write fails.

Saved evidence, extraction manifests and bytes:
`/home/pexserver/Documents/PEXData/REA/top-1.26.52.3/`.

LeviLamina getOrCreateCommand uses service::getCommandRegistry(isClient),
registry.registerCommand, then SyncLocal on client registrations. This example
instead adds a narrow immutable SDK ClientCommands table and packet interception;
no native registry layouts/parse callbacks/command autocomplete are guessed.
Only registered names are consumed. No packet pointers or ECS objects move to
workers. The bounded mailbox records the world epoch and stale commands expire.

Safety is conservative, not a full Minecraft physics simulation: body cells must
be air; full support requires existing native solid flags and an explicit vanilla full-cube
identifier allowlist. Partial/hazardous/custom and unlisted support types are rejected. Runtime
safety, integrated/multiplayer command routing, server corrections and Japanese
chat display require live validation. A build success establishes none of these.
