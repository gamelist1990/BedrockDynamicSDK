# BedrockDynamicSDK data format (schema v1)

Per version there is **exactly one JSON file**: `data/<version>/windows-x64.json`.

- `schema: 1`
- `target`: `product`, `kind`, `platform`, `arch`, `version`, `sha256`,
  `text_sha256`, `image_base`, `size_of_image`, `text_rva`, `text_size`.
- `symbols`: function names to `rva`, `entry_check`, `signature`, `status`,
  `source`, `function_boundary`, `category`, `abi_verified`.
- `virtual_methods`: virtual method names to `slot`, `status`, `source`, and optional
  `abi`. The resolver returns slots only after an exact executable hash match; callers
  must still validate the instance vtable and use candidate slots only when explicitly
  allowed. A slot is not an AOB signature and must be refreshed for each affected build.
- Native sections use **SDK capability names**, never example/mod names.
  See [native section layout](native-profile-layout.md) for their order and ownership.
- `native_keyboard`: verified callback-context offsets, processor virtual slots,
  and InputOrigin for the native gameplay keyboard route. See
  [native keyboard input](native-keyboard-input.md).
- `analysis.rtti`: compiler-emitted decorated TypeDescriptor names; **not** field layouts.
- `analysis.latite_compatibility`: historic 54-pattern comparison for forensic review.
- `analysis.layout_hints`: unverified historical offsets and virtual slots, **never used
  by the runtime resolver**.

The `manifest.json` has `versions[version]["windows-x64"]` entries with `path`,
`sha256` of the JSON itself, `module_sha256`, `module_text_sha256`,
`size_of_image`, `text_rva`, `text_size`, and symbol/RTTI counts.

Runtime selection requires a manifest entry that matches the loaded game by file SHA-256
or mapped code SHA-256 **plus the exact PE layout**. The path must be
`data/<exact-version>/windows-x64.json`, and is retrieved from the pinned HTTPS host
`raw.githubusercontent.com`. The downloaded JSON must match the manifest's SHA-256,
its own target, and the live PE identity. Unrecognized/invalid metadata are skipped.

AOB matches, RTTI and historic SDK offsets do **not** establish runtime ABI safety.
AutoSprint observes the complete `GameCore::keyboardEvent` callback and sends Ctrl
through the actual HIDController key-down/key-up APIs using the profile's verified
`native_keyboard` layout. `Keyboard::feed` is a prefilter, not a gameplay input sender.

## Modular runtime APIs

`bedrock::events` and `bedrock::logging` are independent header-only CMake
INTERFACE targets. Neither downloads symbol JSON, creates background threads,
installs hooks, or changes Minecraft memory. `bedrock::symbols` is optional.
For persistent logs, opt in to `bedrock::FileLogSink`; it truncates the prior
run by default. Append mode must be explicitly requested.
