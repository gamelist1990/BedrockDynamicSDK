# Embedded shared hook runtime

World lifecycle support uses runtime **3.0.0**, shared capability ABI **3**.
After upgrading, restart Minecraft and rebuild participating SDK mods together.
An older owner in the process rejects the incompatible capability; the SDK
does not elect a competing owner. See [world events](world-events-and-packets.md)
for exact-build source validation, candidate status and event subscriptions.

Each Mod statically embeds `bedrock::hook_runtime` and MinHook; there is no
`BedrockSDKRuntime.dll` dependency. On first API use, the embedded copy creates
a named, process-local shared mapping and becomes the MinHook owner. Copies in
other Mods connect to that mapping and forward registration, dispatch, and
removal through a versioned C ABI table. The owner DLL is pinned for the
process lifetime, so registered callback and trampoline addresses remain valid.
Only Mods using this SDK broker are coordinated; third-party code that calls
MinHook directly can still conflict.

Register a middleware callback with
`BDS_HookSubscribe(point, verified_address, callback, user, &token)`. A callback
may call its supplied `next(frame, args)` zero or more times to continue to
later subscribers and then Minecraft's original function; omitting `next`
consumes the call. `BDS_HookUnsubscribe(token)` waits for in-flight callbacks.
Consumers should keep their own module resident until unsubscribe completes.

For hook points not covered by the middleware API, use
`BDS_HookDirectInstall(target, detour, &original, &token)` and
`BDS_HookDirectRemove(token)`. This still uses the process-wide MinHook owner.
It rejects a target already registered through middleware or another direct
hook, rather than silently replacing a detour. It does not make arbitrary
Minecraft addresses safe: callers must resolve and verify targets as usual.

`BDS_HookDispatch` re-enters the full chain for SDK-generated input created
outside a current callback. `BDS_HookCallOriginal` bypasses middleware and
invokes only Minecraft's trampoline; use it sparingly for synthetic events
that must not be observed recursively by the originating feature.

If a guarded entry no longer matches because this broker already patched that
same target, `bedrock::resolveSharedHook` accepts the exact-build verified RVA
only after `BDS_HookOwnsTarget` confirms ownership. Unknown patches and
conflicting target addresses remain rejected.

The shared middleware hook points currently cover `Keyboard::feed`,
`MouseDevice::feed`, `ClientInstance::grabCursor/releaseCursor`,
`LevelRendererPlayer::getFov`, and the TabList level sub-tick callback. The C
ABI avoids sharing STL/CRT state across independently built Mod DLLs. Direct
MinHook is also available through the safe broker API above.

The Windows CI builds two separate test client DLLs, each containing its own
static runtime copy, then loads both into one process and verifies they share
the same hook chain. That is the key co-load guarantee; an in-game Windows test
is still advisable after SDK changes.
