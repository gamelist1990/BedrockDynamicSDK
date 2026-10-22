# Game API

`bedrock::game_api` is a small, Bukkit-inspired C++ facade over Bedrock client
state. It keeps offsets and ABI-specific reads out of mod code. A build-specific
adapter implements `GameAccess` using only validated accessors; an unavailable
operation returns `false`/`std::nullopt` instead of guessing a layout.

```cpp
#include <bedrock/game_api.hpp>

bedrock::GameApi game(access);
auto self = game.localPlayer();
if (self) {
    const auto name = self.getName();
    const auto world = self.getWorld();
    if (auto server = self.server())
        logger.info("server=", server->address());

    game.Event.Enum.Keyboard.then([](const bedrock::KeyEvent& event) {
        if (event.inputKey() == 87 && event.pressed)
            event.cancel(); // suppress this Minecraft input event
    });
    game.Event.Enum.ActorNameTag.then([](const bedrock::ActorNameTagEvent& event) {
        // event.nameTag is borrowed; copy it if it must outlive this callback.
    });

    self.getKeyboard().tap(87);
    self.getMouse().click(bedrock::MouseButton::Left);
    self.turn(3.0f, 0.0f); // yaw/pitch delta, if supported by the adapter
}
```

`Actor::forActor(game, nativeActor)` wraps a native Actor. `LocalPlayer::forActor`
returns a `LocalPlayer` only when that Actor is the current local player.
`Actor::getName()` strips Bedrock `§` formatting and, when the current world's
PlayerList has a partial match, returns that canonical PlayerList name.
`Actor::isPlayer()` uses the same match. The longest matching PlayerList name is
chosen to avoid a shorter account name winning over a longer one.
`GameApi::world(nativeWorld).getPlayerNames()` exposes the current world's
PlayerList names through the adapter, so consumers do not need a second native
PlayerList parser.

## Observations and events

Adapters report data with `observeActorNameTag()` when `Actor::getNameTag` is
observed and `observeViewNameTag()`/`observeViewActor()` when a validated View
source yields an Actor. The two sources are merged by `GameApi::actors()` and
`World::getActors()`. `ViewNameTag` fires only for the View source;
`ActorNameTag` fires for either source and includes `NameTagSource`.
Name-tag observations update the cache, so changes to colored tags replace old
values; tag events are deduplicated and fire when the value changes, not once per
render/frame. Event listeners can be registered with `.then()` and removed with
`.off(id)`/`.unsubscribe(id)`.
Call `clearObservedActors()` when the adapter observes a world/session teardown
so native pointers from the previous world are discarded.

The SDK provides the View ingestion API, but does not currently claim to
enumerate the game's View/Actor list automatically. The old runtime-actor-list
candidate caused invalid-handle SEH failures in the tested build, so it is not
called here. Until a version-verified View adapter is supplied, automatic
collection comes from the `Actor::getNameTag` hook and direct adapter calls.
Use `cachedActors(NameTagSource::View/ActorGetter)` and
`cachedActorNameTags(NameTagSource::View/ActorGetter)` for immutable,
source-separated snapshots. They are rebuilt only after observations change,
so render code can retain a snapshot without re-enumerating Actors. A client
may call `refreshCachedActorNameTags(64)` from its game update (for example,
once every 20 ticks) to refresh a rotating, bounded batch of already observed
tags, including the local player when its `GameAccess` adapter supplies one.
It does not discover new Actors; new handles still come from View/getter observations. Callbacks remain
synchronous on the emitting game/render/input thread; keep them short.

## Native input

`GameHooks::installInput()` subscribes to the shared `Keyboard::feed` and
`MouseDevice::feed` MinHook points. Returning `event.cancel()` prevents the
original Minecraft input callback from receiving that event. This is not
Win32 input injection; `KeyEvent::inputKey()` reports Minecraft's `InputKey`
value (the legacy `virtualKey` field name is retained for source compatibility).
`Keyboard::press/release/tap`, `Mouse::click/move`, and
`LocalPlayer::turn` call the adapter's Minecraft-side methods; the base
`GameAccess` deliberately returns `false` for methods without a verified native
route. Keyboard injection can use the SDK's `NativeKeyboardInput`, captured from
the real `GameCore::keyboardEvent` path, rather than synthesizing OS input.

Hooks are opt-in:

```cpp
bedrock::GameHooks hooks(game);
std::string error;
if (!hooks.installInput(resolver, error))
    logger.warn("input hooks unavailable: ", error);
```

`installActorNameTag()` can be enabled separately. `install()` requires both
input and Actor-name-tag symbols. Hook callbacks and API listeners execute
synchronously; do not block or perform long-running work there.

## Profile and log maintenance

The exact-build profile at `data/<version>/windows-x64.json` contains only the
symbols/layout data needed by the SDK and shipped examples. The generated
`include/bedrock/generated_symbols.hpp` and `data/manifest.json` must be
regenerated with the repository's `bedrock_symbols` CLI after profile edits.
Keep signatures, entry checks, ABI status, and provenance alongside each symbol.

For compact mod logs, `Logger` defaults to `Info`; set `Debug` only while
diagnosing a problem. `FileLogSink::open()` clears the previous run by default; opt into
`Append` only when retaining history is intentional. Prefer one startup summary,
state transitions, and actionable failures over per-frame or per-input-event
messages.
