# World lifecycle events and Packet IDs

## Subscribe from an ordinary SDK mod

Link `bedrock::game_hooks`, load and attach the exact-build profile as usual,
and keep `GameApi` and `GameHooks` alive while subscribed:

```cpp
#include <bedrock/runtime/game_hooks.hpp>
#include <bedrock/enums/packet_id.hpp>

bedrock::GameApi api{access};
bedrock::GameHooks hooks{api};
api.Event.Enum.WorldChange.then([&](const bedrock::WorldChangeEvent& event) {
    // Clear feature-owned names, retained positions and queued snapshots here.
    // Copy event.epoch into worker jobs; discard results whose generation
    // no longer equals api.worldEpoch(). Keep native pointers on the game thread.
});
api.Event.Enum.DimensionChange.then([](const bedrock::DimensionChangeEvent& event) {
    // Native notification, including changes with an unchanged Level pointer.
});
std::string error;
if (!hooks.installWorldEvents(resolver, profileJson, error)) {
    // Report error and stop the feature requiring lifecycle detection.
}
```

`WorldChange` reports `Joined`, `Left`, `JoinCancelled` or `DimensionChanged`.
Typed channels are `ClientJoinLevel`, `ClientExitLevel`, `ClientCancelJoinLevel`
and `DimensionChange`. Every notification advances `api.worldEpoch()` and
invalidates SDK names, Actor reads and render-view caches before callbacks.
Joined/dimension callbacks follow the native method; exit/cancel callbacks
precede teardown. General and specific channels share one generation.
Callbacks execute synchronously on the native caller thread; enqueue copied
values for workers and keep callbacks short.

Handles are borrowed and may be null. `level` is the joined player's level
when available, or the last observed `LevelTick` level for a boundary. Install
`installLevelTick` if that optional last-level value is needed. The native
dimension callback has no dimension-ID parameters; none are fabricated.
The first notification binds the client for that session, and exit/cancel
releases the automatic binding. For split-screen, pin a client using
`api.setWorldEventClient(client)`; pass `nullptr` to restore automatic selection.

HitBox installs this source, advances its worker/render epoch at each boundary
and immediately hides old boxes, retained positions and names. Its profile
selection now requires the lifecycle layout, so an old cache cannot silently
omit this source. A same-dimension teleport between server arenas need not
be an engine world lifecycle event; server-internal map identities are not
inferred by this API.

## Native source and limits

LeviLamina is a reference; these SDK hooks do not require that loader.
Reference revision:
[`7c0f7d125a80274f406c0be8a9a17e4d9cbb7801`](https://github.com/LiteLDev/LeviLamina/tree/7c0f7d125a80274f406c0be8a9a17e4d9cbb7801).

| Native method | Source / native table | 1.26.52.3 RVA |
| --- | --- | --- |
| `onPlayerLoaded(Player&)` | ClientJoinLevelEvent emitter; IClientInstance slot 327 | `0x5DD06F0` |
| `onLevelExit()` | ClientExitLevelEvent emitter; GameCallbacks slot 7 | `0x5DB42F0` |
| `onCancelJoinGame()` | ClientCancelJoinLevelEvent emitter; IClientInstance slot 13 | `0x5DA38F0` |
| `onDimensionChangedEvent()` | IClientInstance declaration, slot 134 | `0x5DBE5D0` |

Primary table `0xE9731B0` was located through independently named
requestLeaveGame methods at slots 14/15 (`0x5DA3BD0` / `0x5DA3DE0`); slot 31
matches the local-player getter. The dimension body resolves
DimensionTypeComponent by FNV-1a hash `0xAF993816`, corroborating the declaration.
Secondary GameCallbacks table `0xE973F20` has exit at slot 7. Its body adjusts
RCX by `-0x88`; `native_world_events.exit_client_offset` records that correction.
The middleware forwards the original subobject pointer unchanged to Minecraft.
Primary-table indexing must not be used for this secondary-table method.

All four profile entries retain **candidate**, `abi_verified: false` and
`runtime_verified: false`. Static prototypes and instruction registers match
the reference; native invocation remains unobserved. Installation requires an
attached exact executable fingerprint and entry guards, or the same address
already owned by the broker. An AOB-only match is never installed as a world
detour. Registration preflights all four targets and rolls back only this
group on failure.

Target SHA-256:
`4a92bfa3ce2428b40ee517b7c1125d9f6f79274382de03846351992663b2e2e4`.
REA/Ghidra function evidence IDs:

- Join: `ev_622de34172a5e3a8e479640ebd3f302fb6d37c7ea8c46c381aa827812dd01962`
- Exit: `ev_7b03e9eeb4ba609550569d2e257d863ae29dc50167edbb751b29fcc757acc096`
- Dimension: `ev_27d5015c85c5e0c8b4740f32926e7512c8c43ad585d894bb64cff23b06ec5e7a`
- Cancel: `ev_f412fe40a7569c670e78130b19f4765c0f2712608162e58b2c70cf46426e0cf3`

Evidence files remain outside the repository under
`~/Documents/PEXData/REA/world-events-1.26.52.3/`. Raw-function imports lack PE
metadata and use Ghidra's generic x64 calling convention; pseudocode argument
ordinals are not Windows ABI declarations. Windows registers were checked
against assembly. CFG indirect calls are unresolved. Join is a leaf tail
dispatch without its own .pdata range. Cancel's final branch leaves the slice,
causing a truncated-control-flow warning. No live Minecraft execution was
observed on this Linux host.

## PacketList enum

```cpp
using bedrock::PacketList;
auto id = bedrock::packetIdFromValue(61);
if (id && *id == PacketList::ChangeDimension) {
    auto name = bedrock::packetName(*id); // "ChangeDimension"
}
auto slotId = bedrock::packetIdFromName("InventorySlot");
static_assert(bedrock::packetIdValue(PacketList::InventorySlot) == 50);
```

`PacketId`, `PacketList` and `MinecraftPacketIds` alias the same scoped uint16
enum. Its 253 named protocol IDs come from the pinned reference's
[`MinecraftPacketIds.h`](https://github.com/LiteLDev/LeviLamina/blob/7c0f7d125a80274f406c0be8a9a17e4d9cbb7801/src/mc/network/MinecraftPacketIds.h).
These numeric identifiers do not describe native object/payload ABI or promise
every packet exists in every game version. Deprecated entries keep their names.
Private LeviLamina ID 201, range markers and the end sentinel are excluded.

Numeric/name conversion returns `std::nullopt` for unknown or reserved values;
preserve the original numeric value if an observer needs it. `packetName`
returns `"Unknown"` for an unrecognized enum value. Friendly aliases include
`MobEquipment`, `LevelChunk`, `ModalFormRequest`, `PlayerAuthInput` and
`SetLocalPlayerAsInitialized`; output names are canonical. `packetIdFromHeader`
extracts the low 10 ID bits from an already-decoded wire VarInt header.
`packetIdFromValue` checks range before narrowing and never masks a header.
This header installs no hooks, sends no traffic and has no runtime dependencies.
