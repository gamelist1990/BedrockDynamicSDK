# View-based Actor enumeration investigation

This note records the 1.26.52.3 Windows x64 investigation. It does not mark a
new Actor enumeration adapter as runtime-verified.

## Executable identity and actual-client evidence

The decrypted PE used by the SDK profile is
`local-binaries/1.26.52.3/Minecraft.Windows.exe`:

```text
SHA-256:       4a92bfa3ce2428b40ee517b7c1125d9f6f79274382de03846351992663b2e2e4
.text SHA-256: 6fa218d247874940235aacb8fd7e3bbf0e3d1e259630d7f83fcb536f06733aeb
.text RVA:     0x1000
.text size:    240,802,816 bytes
```

The Bedrock-on-Linux installation copy is encrypted at rest and is not a PE on
disk. Its launcher maps a decrypted image from `/dev/shm`; the runtime profile
cache for the active client contains the `.text` hash above, and the live
HitBox log says the embedded 1.26.52.3 profile was selected. Therefore the
decrypted profile-matched PE is the available static-analysis input. Directly
reading the process mapping was not possible under the host's ptrace policy.

Ghidra imported the PE and its `.pdata` (944,456 function ranges), but the
default analysis timed out at the configured 900-second limit. The separate
static scan of the exact `.text` found the component IDs below. The SDK's
current HitBox runtime log reported `actor_count=1`; this proves that the
existing registry adapter ran without an exception in that session, but does
not prove that it enumerated a populated multiplayer world.

## Exact-build EnTT component identifiers

The executable contains the 32-bit FNV-1a hashes of the bare component names.
The counts below are raw occurrences in `.text`; these are useful corroboration
for the existing profile's primary values, not proof of a particular registry
pointer or query ABI.

| Component name | Hash | `.text` occurrences |
| --- | ---: | ---: |
| `ActorOwnerComponent` | `0x85B93800` | 1,710 |
| `ActorTypeComponent` | `0x4F6BA419` | 77 |
| `RuntimeIDComponent` | `0xFC0DBBB5` | 39 |
| `AABBShapeComponent` | `0xBAC1B3CF` | 202 |
| `StateVectorComponent` | `0x1B5D5238` | 583 |
| `ActorRotationComponent` | `0x75DF36B7` | 212 |

The older `class ...` / `struct ...` hash alternatives from Solstice are absent
from this executable's `.text`. For example, `ActorOwnerComponent` hashes to
`0x85B93800` as a bare name, while the older `class ActorOwnerComponent`
alternative is `0x132A5818` and had no `.text` hit. The 1.26.52.3 profile now
sets these unsubstantiated alternates to zero, so they cannot silently become
runtime fallbacks.

The binary has a large function at RVA `0x03284500` which materializes current
component hashes including `ActorTypeComponent`, `RuntimeIDComponent`, and
`AABBShapeComponent` while resolving component storage. Another function at
RVA `0x021669A0` resolves `ActorOwnerComponent` and `AABBShapeComponent` for an
entity. These are static disassembly observations, not verified entry points
for an external caller and not a signature/ABI for a whole-world list API.

## Enumeration route and terminology

There are two different meanings of “View” in the available sources:

1. **Entity-registry view**: iterate the Level's EnTT registry and intersect
   Actor component storages. The attached Solstice 1.21.111 source uses
   `registry.view<ActorOwnerComponent, ActorTypeComponent,
   RuntimeIDComponent, AABBShapeComponent>()` in `ActorUtils::getActorList`.
   This is the best candidate for discovering live Actors that have not yet
   called `Actor::getNameTag`; it is broader than an observation hook.
2. **Render view**: current 1.26.52.3 strings include
   `LevelRendererCamera::render(..., const ViewRenderObject&, ...)` and
   `queueRenderEntities`. This is a rendering/culling path, not an authoritative
   world Actor roster. It may omit actors outside the current render set and
   should not be treated as the all-Actor source.

The SDK now has a separate render-pass publication API:
`GameApi::publishRenderViewActors(viewObject, frame, handles)` replaces a
bounded immutable snapshot, `cachedRenderViewActors()` reads it without
enumerating the world, and `Event.Enum.RenderViewActor` reports unique handles
from that pass. This API is designed for visible/culling candidates only and
does not merge those candidates into `GameApi::actors()`.

This is an adapter boundary, not an automatic Minecraft hook yet. Static
strings establish that the named function exists in the exact executable, but
do not establish its exact RVA, calling convention, `ViewRenderObject` field
layout, or an Actor-pointer list within that object. A build-specific adapter
must first resolve the render pass's actual candidate Actor handles and then
publish them; passing `ViewRenderObject*` as an Actor is invalid. Until that
entry and data path are validated, the SDK leaves the native hook disabled.

For Actors whose name tags the renderer requests, the render hook is not needed
to obtain their pointers: the existing exact-build `Actor::getNameTag` hook
receives the Actor as its `this` pointer and publishes it through
`ActorNameTagEvent` / the `ActorGetter` cache. TabList already uses this route.
The client now refreshes its MurderDetect roster on the next Level tick after a
new name-tag observation instead of waiting up to one second. This provides a
low-latency roster of observed name-tag Actors, not a complete world list;
actors whose name tags are never queried remain undiscovered.

LeviLamina's current client headers are useful corroboration but not a
substitute for the pinned 1.26.52.3 binary: they declare `LevelRendererCamera::render`
with `BaseActorRenderContext&`, `ViewRenderObject const&`, and
`IClientInstance&`, and put `mActorRenderCandidates` on `LevelRendererCamera`.
Its `ViewRenderObject` fields instead include render collections such as actor
shadows and name tags. This makes the name-tag getter a simpler, already
integrated source for visible player Actors; it does not establish that the
1.26.52.3 `ViewRenderObject` itself contains an Actor pointer array.

The SDK `GameApi::actors()` merges a native
`GameAccess::actorHandles(world)` source with its Hook/View observations. The
HitBox adapter now implements the dynamic-storage equivalent of
`registry.view<ActorOwnerComponent, ActorTypeComponent, RuntimeIDComponent,
AABBShapeComponent>()`:

1. It obtains the registry from the current LocalPlayer's EntityContext.
2. It resolves the four exact-build storage IDs from the selected profile.
3. It walks the smallest available storage and checks membership in Owner,
   RuntimeID, and AABBShape, with `registry.valid(entity)` before accepting a
   row. ActorType is an additional filter when that storage resolves in the
   live registry; if it does not, the adapter uses this validated three-pool
   fallback instead of disabling the whole source.
4. It reads ActorOwner only after the full intersection, then checks the
   Actor's vtable, registry pointer, and raw entity ID against the row.
5. The resulting Actor handles are passed through the normal SDK
   `GameApi::actors()` facade; bounds/position/rotation remain component reads.

One compact `native Actor EnTT view` diagnostic reports each storage size,
candidate rows, valid entities, intersection size, and accepted pointers. This
is deliberately a one-time diagnostic rather than per-frame logging. The
profile parser now requires `ActorTypeComponent` and `RuntimeIDComponent`; the
new parser test checks both successful exact-profile parsing and rejection of
a profile that cannot form the required view.

This is implementation progress, not a verified claim of complete runtime
enumeration. The present HitBox caller requests `GameApi::actors()` from its
DXGI render callback; the registry query is therefore still synchronous on
that caller's thread. Before marking this runtime route verified, move or
confirm the query on a safe game-thread/tick boundary and test in a populated
world: compare view matches with visible and non-rendered Actors, validate
unique IDs over multiple ticks, and watch for stale actor handles or SEH. The
prior `Level::getRuntimeActorList` call remains disabled; no Level method call
is used by this implementation.

Do not replace the storage walk with a compile-time EnTT `view<T...>()` until
the external component type IDs and the game's EnTT layout are confirmed to
match. Do not promote `native_actors` to `verified` until an in-world test with
multiple nearby and non-rendered Actors confirms stable counts, valid
ActorOwner pointers, unique entity IDs, and no SEH/lockup. The existing
`Level::getRuntimeActorList` invocation remains disabled because prior builds
raised SEH exceptions.
