# Embedded shared hook runtime

The current runtime is **4.3.0**, shared capability ABI **4**. Its SharedApi
function table is unchanged from ABI 4, so existing ABI 4 mods can use a newer
owner. World lifecycle support originally shipped with runtime 3.0.0 / ABI 3.
Restart Minecraft after upgrading an already active owner. Unsupported
capabilities fail locally; the SDK does not elect a competing owner. See [world events](world-events-and-packets.md)
for exact-build source validation, candidate status and event subscriptions.

Each Mod statically embeds `bedrock::hook_runtime` and MinHook; there is no
`BedrockSDKRuntime.dll` dependency. On first API use, provider discovery selects a runtime to create
a named, process-local shared mapping and become the MinHook owner. Copies in
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
the embedded copy remains the fallback in 4.1/4.2 (4.3 also discovers loaded
providers, below). Startup happens outside DllMain and
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

## Runtime 4.2.0

Compatible table ABI remains 4. The supported-point mask now also includes
point 14, `BDS_HOOK_PACKET_SEND_TO_SERVER`, arguments `BDS_PacketSendArgs`.
A subscriber can omit continuation to suppress a packet; it must not retain
the borrowed packet pointer. Freecam only suppresses its local sender's native
movement traffic while a Borion proxy needs restoration. Legacy ABI 4 owners
without this capability cause that feature to stay unavailable.


## Runtime 4.3.0: loaded module provider discovery

Every DLL linked to `bedrock::hook_runtime` now exports the provider protocol
from `src/hook_runtime.cpp`; the optional sidecar wrapper contains only DllMain.
MSVC consumers inherit `/INCLUDE:BedrockHookRuntime_BuildVersion` (with the
32-bit C decoration where applicable), forcing extraction of the static runtime
object even when the consumer otherwise makes no runtime API calls.

Provider exports use the C calling convention:

- `unsigned BedrockHookRuntime_BuildVersion()`: side-effect-free packed
  major/minor/patch (`0x00040300` for 4.3.0).
- `unsigned BedrockHookRuntime_BuildAbi()`: side-effect-free exact provider
  table ABI, currently 4.
- `int BedrockHookRuntime_Start()`: synchronously initialize or attach through
  `getSharedApi(false)`, explicitly bypassing discovery. Returns nonzero when
  attached to a compatible owner; it does not promise that this DLL won election.
- `const char* BedrockHookRuntime_Version()` and
  `int BedrockHookRuntime_Supports(unsigned point)`: report the actual owner
  using the public runtime APIs, which initialize discovery when necessary.

Before first owner election, an ordinary API call optionally loads the exact
`BedrockHookRuntime.dll` path beside its own consumer, then takes a Toolhelp
snapshot of already loaded modules. It takes transient module references before
reading provider exports and retains compatible candidates through Start and
owner attachment. Snapshot unload/address-reuse races are filtered by path.
No unloaded Mod DLL is searched for or loaded. Only the optional exact-path
sidecar is loaded; Windows resolves its dependencies using the explicit DLL
load-directory/default-directory flags, without a CWD/PATH search for the sidecar.

Selection requires exact provider ABI 4 and a build at least as new as the
caller's embedded runtime. Highest version wins; equal versions prefer a DLL
named `BedrockHookRuntime.dll`, then sort by full path case-insensitively with
an ordinal spelling tie-break. The local runtime is the fallback. Provider Start
runs outside the caller's shared-table mutex. Bypassing discovery in Start
prevents cross-module A -> B -> A bootstrap lock/call_once recursion.

The existing process mapping always wins, including an owner created by a
legacy ABI 4 mod. Discovery never resets/replaces that table or forks a broker,
even before any hooks have been installed. The SharedApi table and all argument
records remain unchanged. ABI 3 attachment retains the existing supported-point
checks, but ABI 3 providers are not elected by discovery. Failed initialization
leaves no successful local cache and may be retried on the next API call;
snapshot ERROR_BAD_LENGTH retries are bounded. Concurrent initializations may
observe different loaded-module sets: the first published owner remains final.

`BDS_HookRuntimeDiagnostic()` initializes the runtime if necessary and returns a
UTF-8 snapshot valid until the same thread's next diagnostic call. Initialization
also sends the diagnostic to `OutputDebugStringW`. It includes the attempted
sidecar path and LoadLibraryEx GetLastError code, selected provider version/path,
actual owner version/ABI/path, and a restart requirement when the actual owner
is older or unknown. Successful initialization caches its table; hook calls do
not enumerate modules, format diagnostics, write disk logs, or start log threads.
Failed attempts can emit another diagnostic when retried.

Discovery only considers providers loaded at initialization. Load all desired
mods before their first runtime API call, or explicitly load the preferred
provider and call Start first. A newer mod loaded after owner publication cannot
take over; restart with that provider available earlier. Old binaries without
provider exports cannot advertise their embedded build. Old binaries also cannot
perform this new discovery, so a legacy mod that initializes first can still
become the owner. Toolhelp failure/unload races can omit providers; the local
fallback and one-owner rule still apply. Explicit Start intentionally skips
ranking: loaders calling it are choosing which runtime attempts election.
