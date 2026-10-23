# ActorUniqueID static analysis: Minecraft 1.26.52.3

Target: `Minecraft.Windows.exe`, SHA-256
`4a92bfa3ce2428b40ee517b7c1125d9f6f79274382de03846351992663b2e2e4`, image
base `0x140000000`. The executable matches the binary recorded by the supplied
ScanClient archive.

## Findings

The client has three different identifiers that must not be conflated:

| Identifier | Representation | Meaning |
| --- | --- | --- |
| registry entity ID | 32-bit entity handle | EnTT/ECS slot plus generation; exposed by `Actor::getEntityId()` |
| ActorUniqueID | 64-bit component value | Stable Bedrock actor identity; exposed by the new `Actor::getUniqueId()` adapter API |
| ActorRuntimeID | 64-bit component value | Runtime/network actor identity; exposed by `Actor::getRuntimeId()` |

The exact executable contains the compiler-generated type name
`ActorUniqueIDComponent`, and code uses its FNV-1a component hash `0x18f957af`.
At VA `0x1419f2de0`, the code resolves this component through the entity
registry. The component value's first eight bytes are checked as a 64-bit
sentinel; when initialization is needed, the code obtains an eight-byte value
through a virtual dispatch and stores it at the start of the component value
(`mov %rcx,(%rsi)` at VA `0x1419f2eeb`). This supports a candidate layout of
`ActorUniqueIDComponent { uint64_t value; }`, with value offset `0` and size `8`.

The nearby function at VA `0x1419f2920` probes for the same component. The
RuntimeID component uses a separate hash, `0xfc0dbbb5`; for example, code at VA
`0x1419f2a00` checks its component independently. This confirms that RuntimeID
is not the ActorUniqueID component.

The EnTT `entity_id_offset` in the profile remains the registry handle and
must not be reinterpreted as ActorUniqueID. The existing `getEntityId()` API
keeps that meaning for compatibility. `getUniqueId()` was added as a separate
optional adapter read and uses the existing per-Actor read cache.

## Limits and implementation status

The profile now records `actor_unique_id.component_hash = 0x18f957af`,
`value_offset = 0`, and `value_size = 8`, but the section remains marked
`candidate`. Static disassembly does not establish a safe generic native
registry reader, the complete ABI of the virtual dispatch, or correctness while
actors are being removed. The new SDK API therefore does not read arbitrary
process memory itself: a build-specific `GameAccess::actorUniqueId()` adapter
must supply the value. Validate it against live actor handles and the game
thread before enabling it for matching across sources.

The virtual dispatch observed during component initialization is not promoted
to a public `Actor` method or vtable slot. Its receiver and ABI need separate
validation. ScanClient's signatures and path snapshots do not provide a
verified `ActorUniqueID` getter.
