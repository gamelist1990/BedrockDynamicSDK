# OreUI Mod Settings — 1.26.65.1 Windows x64

Build from the SDK root:

```sh
./build-linux.sh --oreui-settings
```

Load only `dist/1.26.65.1/ModSettingsDemo.dll` after the main menu is visible.
The adapter waits for the OreUI View to update, calls `ExecuteScript` on that
thread, and waits for a marker on the local bridge before installing the
Settings query/command adapter. It never replaces or reloads the shipped
HTML or JavaScript. The resource hooks answer only the SDK's per-process
bridge prefix; other requests go to the original handlers.

No external asset or runtime DLL is required. View discovery and bridge
activity are recorded in `%LOCALAPPDATA%\BedrockDynamicSDK\ModSettings\oreui.log`.
The demo logs committed values to `ModSettingsDemo.log` beside the DLL.

For a fresh session, open **Play**, return to the title without joining a
world, then open **Settings → Mod Menu → Settings Demo**. This lets OreUI
start before its Settings screen subscribes and caches command bindings.
Opening Settings first can miss the new sidebar on that visit or leave its
controls bound to vanilla commands, even after reopening. Restart and use the
**Play** path if that happens.

| ID | Control | Initial state |
| --- | --- | --- |
| `enabled` | Boolean toggle | On |
| `intensity` | Number slider | 50; range 0–100; step 5 |
| `mode` | Option selector | Safe; also Fast / Precise |
| `message` | String input | `Hello OreUI`; committed when editing ends |
| `ping` | Action button | Logs a ping; not saved |
| `about` | Text | `SDK Settings Demo` |
| `preview` | Image | 240×135 crystal PNG |

The SDK persists values in `%LOCALAPPDATA%\BedrockDynamicSDK\ModSettings\mod-settings-demo.json`.
The demo reads the persisted values on registration; text and image are read-only.

## Internal adapter contract

This first-party diagnostic exercises the internal OreUI transport, including
image and action controls. Ordinary Mods use `bedrock::sdk` and
`bedrock::sdk::SettingsPage` instead. Register pages outside `DllMain`.
The adapter copies descriptors and image bytes. Use `readValue` to apply loaded
values in the mod. Call `unregisterPage` before unloading a callback's owner.
The DLL that owns the shared adapter stays resident until process exit. Callbacks run on
the OreUI resource request thread, not the game thread.

IDs are 1–64 lowercase ASCII letters, digits, hyphens or underscores. Setting
IDs on the bridge are `bedrockdynamic.mod.<mod-id>.<control-id>`; native
`OptionID` is unchanged and the adapter forwards vanilla commands. The SDK
accepts up to 16 pages, 64 rows per page and 2048 UTF-8 bytes per String.
Options persist by stable choice key, not by display label or numeric index.
If storage fails, the SDK leaves the live value unchanged.

## Image

The demo embeds a PNG in the DLL and passes the bytes to the SDK. The
adapter inserts the decorative image into its native Settings text row.

## CheckSig and runtime verification

`checksig-oreui-probe.json` probes both native candidate entries. Export
`oreui-profile.json` alongside it into the CheckSig commands directory:

```sh
python3 tools/profile_parts.py assemble data/1.26.65.1/windows-x64/manifest.json \
  --output /path/to/CheckSig/commands/oreui-profile.json
```

Run the probe **before** injecting the demo so it sees
unpatched entries. The host CheckSig profile test and offline PE resolver
do not verify the in-process response ABI, XHR bridge, or image rendering.
Those are BOL verification items. The profile as a whole remains `candidate`;
the 1.26.65.1 Cohtml `ExecuteScript` slot is independently corroborated by
the live vtable and the script's XHR marker. BOL may enlarge the executable's PE stack reserve; CheckSig
requires a test-only profile with the staged file SHA-256 in that case. Compare
the two executables first. Do not replace the canonical profile SHA-256.
