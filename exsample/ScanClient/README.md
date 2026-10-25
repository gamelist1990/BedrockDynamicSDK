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
- `world_transition_strings.json` and `world_transition_references.json`:
  focused world-transition names and code references, kept separate from
  networking clues.
- `client_send_packet_strings.json` and `client_send_packet_references.json`:
  explicit `Serverbound...Packet`/payload names and `PacketSendBeforeEvent`
  clues. The supplied 1.26.52.3 capture includes names such as
  `ServerboundDiagnosticsPacket`, `ServerboundLoadingScreenPacket`,
  `ServerboundDataStorePacket`, `ServerboundPackSettingChangePacket`, and
  `ServerboundDataDrivenScreenClosedPacket`.
- Static string references are emitted only when HDE64 decodes the containing
  instruction at a real boundary reached by linear decoding from a `.pdata`
  function start. Function ranges above 1 MiB or with undecodable bytes are
  counted in `reference_decoder` and can be absent from the results.
- `scan_candidates.json`: ranked function candidates plus pages observed
  changing during runtime monitoring. Entries include an ID, bounded RVA range,
  evidence, and explicit confidence/limitation notes.
- `candidate_exclusions.json`: matching string references excluded from the
  selectable shortlist because their name was not on the specific candidate
  allowlist.
- `candidate_scans/<id>/`: timestamped binary and JSON snapshots made with
  `scan candidate <id>`. Repeating the command records another sample for that
  range without capturing the whole module again.
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

## Candidate-focused developer workflow

After the focused static index is ready, use `candidates` to list the static
shortlist and the most frequently changing pages. Use `candidates dynamic` to
list up to 50 runtime-changing pages, then `scan candidate <id>` to snapshot one
selected function or 4 KiB page. Runtime-page IDs use the form `data-<rva>`;
static IDs are printed by the console. Targeted snapshots use guarded reads and
record the executable fingerprints in their JSON metadata.

Candidate IDs are printed by `candidates` and are generated from the current
executable's function RVAs. Use the ID shown on that run:

```text
candidates
scan candidate <ID printed by candidates>
```

Use the ID whose category is `world-transition` or
`client-to-server-packet-type`; IDs differ between game builds.

Ranking is conservative. A function referencing the specific
`Minecraft::registerLevelListener` signature or player `changeDimension` trace
label is high priority for review, but neither proves that a callback fires on
every world switch. `DimensionChangeProgressHandler` is a lower-priority type
clue. A changing `.data` page remains runtime-change-only until correlated with
a controlled action. Capture samples before and after a world switch to narrow
candidates; verify the call path and ABI before promoting any result to an SDK
symbol.

`PacketSendBeforeEvent` is retained as a send-event API clue; it is not treated
as a native network-send hook. The supplied 1.26.52.3 capture also has explicit
`Serverbound...Packet` and payload names. Their function references point to
type-related code that may help trace serialization or dispatch, but they do
not prove those functions send packets, expose packet bytes, or can be safely
hooked from the client. `PacketReceivedBeforeEvent` is excluded from the
client-to-server candidate list because it describes the opposite direction.

The published `@minecraft/server-net` documentation describes these packet
events as part of a Bedrock Dedicated Server-only module and says the APIs do
not work in the Minecraft game client or Realms. That documents the script API
availability boundary; it does not prove that the client binary lacks an
internal native event path. The documented script event exposes packet ID,
size/sender or recipients and cancellation state, not a raw packet-byte buffer.
See the [module reference](https://learn.microsoft.com/en-us/minecraft/creator/scriptapi/minecraft/server-net/minecraft-server-net?view=minecraft-bedrock-experimental),
[`PacketReceivedBeforeEvent`](https://learn.microsoft.com/en-us/minecraft/creator/scriptapi/minecraft/server-net/packetreceivedbeforeevent?view=minecraft-bedrock-experimental),
and [`PacketSendBeforeEvent`](https://learn.microsoft.com/en-us/minecraft/creator/scriptapi/minecraft/server-net/packetsendbeforeevent?view=minecraft-bedrock-experimental).

HDE64 validates instruction boundaries by decoding from each `.pdata` function
start, which removes the old scan that treated every byte as a possible
instruction. Linear decoding can still stop at unusual embedded data or
unsupported bytes; check `reference_decoder.decode_errors` and
`functions_too_large`. A verified string reference still does not prove runtime
execution, world-transition semantics, an outbound send path, or ABI. Verify
the selected snapshots against the exact executable in REA/Ghidra and correlate
them with controlled runtime actions before adding an SDK symbol.

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
