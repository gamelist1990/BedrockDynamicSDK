# Bedrock ScanClient

ScanClient is a developer-only Windows x64 DLL for read-only inspection of the
current Minecraft Bedrock executable. It writes a versioned bundle to
`%USERPROFILE%/Documents/PEXData/ScanClient/<version>/windows-x64/`.

The scanner does not call game functions or modify game memory. It uses guarded
`ReadProcessMemory` reads on a low-priority worker. It captures only the
Minecraft image's `.text`, `.rdata`, and `.pdata` PE sections; private heaps,
other modules, and unrelated process mappings are excluded. The `.text` bytes
are streamed to disk in chunks instead of being loaded as one large buffer.
There is no duplicate `mapped_image.bin`.

## Output

- `manifest.json`: version, executable fingerprints, and bundle file index.
- `windows-x64.json`: profile-compatible target metadata and candidate counts.
- `module_layout.json`: PE section table and the exact readable extents captured
  from each selected section, including loaded VA and RVA.
- `module_sections/`: captured section extents as `.bin` files. These are
  loaded-memory bytes, not a reconstructed PE executable.
- `world_transition_strings.json`: `.rdata` strings matching world, level,
  dimension, session, packet, transfer, respawn, and unload terms.
- `world_transition_references.json`: recognized x64 RIP-relative references
  from `.text` to those strings, with the nearby instruction bytes and the
  containing `.pdata` function range when available.
- `function_pointer_tables.json`: `.rdata` pointer runs whose targets fall on
  `.pdata` function starts. They are table candidates, not verified vtables.
- `signatures.json`: short exact-build function prefixes associated with
  string references or pointer tables. Uniqueness and ABI are unverified.
- `rtti_name_candidates.json`: matching strings that resemble MSVC RTTI names;
  the required descriptor/type relationships are not established.
- `ghidra_import.py`: imports captured section extents at their observed loaded
  addresses while preserving unreadable gaps.
- `progress.json`: state, bytes read/saved/skipped, and read-failure counts.
- `dynamic_changes.jsonl`: timestamped changed-page observations from the
  repeated runtime monitor, including section RVA, changed byte offsets, and
  before/after prefixes.

The former process-wide scan and generic capped byte/string findings were
removed. Capture is limited to the three executable sections and relevant
candidate output. Analysis buffers are capped at 256 MiB per section; `.text`
is scanned from disk in chunks. Unreadable or uncommitted bytes are recorded as
skipped, and the bundle is marked partial unless every selected section byte
was saved. Pause/resume/stop/status commands are available in the console.
Stopping records completed extents in `module_layout.json`; `restart` begins a
fresh pass and does not resume from an old cursor.

At startup, a second low-priority worker begins monitoring in parallel with
the one-time static capture and continues until `stop`. It repeatedly reads
writable PE sections belonging to `Minecraft.Windows.exe`, keeps a bounded
128 MiB baseline, and checks them again after each pass. Only changed pages are
appended to `dynamic_changes.jsonl`; the console `status` command reports pass
and change counts. `pause` and `resume` suspend and continue both capture and
monitoring. This focused monitor does not repeatedly read immutable code/data
or private heaps, which would add substantial load and noise. It records byte
changes, not semantic game events, and can miss changes that begin and end
between samples.

The string-reference decoder intentionally recognizes only common x64
RIP-relative memory operands. It can miss other encodings and can produce
false-positive references. A string reference or `.pdata` boundary does not
prove that a function is called only during a world change. Verify each
candidate against the exact executable in REA/Ghidra and with runtime evidence
before adding an SDK symbol or relying on its ABI.

No in-process DLL can guarantee that the host game will never crash, but the
scanner avoids direct dereferences, checks PE bounds, uses guarded reads, and
does not invoke game code. It is a local SDK research tool and does not include
a DLL injector.

## Ghidra import

Create an `x86:LE:64:default` program, run `ghidra_import.py` from Ghidra's
Script Manager, and select the `windows-x64` output folder. The script imports
captured extents individually, so unreadable pages do not become fabricated
code or data blocks. Use the original `Minecraft.Windows.exe` for static PE
analysis; the ScanClient bundle supplies runtime bytes and loaded addresses for
cross-checking.
