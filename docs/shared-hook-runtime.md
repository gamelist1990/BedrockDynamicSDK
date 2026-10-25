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

## Runtime 4.0.0

Capability ABI 4 adds `BDS_HOOK_RENDER_LEVEL` with `BDS_RenderLevelArgs`
(renderer, screen, context). Subscribers call the continuation before drawing.
Updated HitBox and Freecam share this hook; all co-loaded consumers require
a rebuild and a complete Minecraft restart after a runtime upgrade.

## Runtime 4.1.0: compatible owners and preferred bootstrap DLL

Runtime version is independent of the table capability ABI. Version 4.1 keeps
the ABI 4 table byte-for-byte, including existing argument records. A new
consumer accepts the inspected ABI 3/4 prefixes and checks points individually:
Legacy ABI 3 supports 1..12; legacy ABI 4 supports 1..13. New owners publish
an independent versioned capabilities mapping with an actual supported-point
mask, build version and owner subscribe-pointer identity. Additive future
points can update that mask without changing the shared function-table ABI. `BDS_HookRuntimeSupports(point)` and
`BDS_HookRuntimeAbi()` report the **active owner**, not the caller's embedded
version. All point-taking exports gate unsupported points before dispatch.
Unknown incompatible ABIs are rejected; a larger integer is not proof of compatibility.

An optional `BedrockHookRuntime.dll` is built by the SDK tools job. Place the
newest compatible release beside the Mod DLLs. Updated consumers load that
exact directory's sidecar, check its side-effect-free BuildVersion/BuildAbi exports,
and synchronously invoke `BedrockHookRuntime_Start`
before first owner election only if it is ABI-compatible and at least as new
as the embedded version. An older sidecar is not activated. No CWD/PATH search is used. Without a sidecar,
the embedded copy remains the fallback. Startup happens outside DllMain and
outside the consumer's shared-API mutex. The owner remains pinned.

For an unrecompiled legacy Mod, a loader can load the sidecar and call
`BedrockHookRuntime_Start` before loading that Mod. Existing ABI 4 consumers
accept the newer ABI 4 owner unchanged. Legacy ABI 3 strict-equality consumers
still reject an ABI 4 owner: modifying a new DLL cannot rewrite their old check.
For those consumers a rebuild is required once.

This chooses the installed sidecar at startup. It neither scans arbitrary
unloaded DLLs for a speculative newer runtime nor replaces an owner after hooks
are active. If an older owner is already active, supported existing features
continue through it and unsupported features fail locally. Restart after
updating the preferred sidecar. Custom loaders which copy DLLs elsewhere must
copy the sidecar too or explicitly load/start it first.
