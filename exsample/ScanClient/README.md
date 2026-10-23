# Bedrock ScanClient

ScanClient is a developer-only Windows x64 DLL that inspects the current
Minecraft Bedrock process and writes a versioned analysis bundle under
`%USERPROFILE%/Documents/PEXData/ScanClient/<version>/windows-x64/`.

It uses the SDK's loaded-game fingerprint (`inspectLoadedGame`) and profile
format. Scanning runs on a dedicated worker and never calls game functions or
hooks/modifies game memory. Each candidate is marked `candidate` / unverified;
discovery is not ABI verification and the generated profile must not be used
to call functions until independently checked.

## Output

- `manifest.json`: SDK-style index with build fingerprint and output files.
- `windows-x64.json`: profile-compatible target metadata and candidate groups.
- `module_layout.json`: PE headers and section table, including section name,
  RVA, loaded VA, raw file offset/size, virtual size, and characteristics.
- `module_sections/`: exact readable bytes copied from each mapped PE section,
  split into files with the RVA in each filename. These are loaded-memory bytes,
  not reconstructed on-disk PE files. The `.text` dump can be hundreds of MiB.
- `mapped_image.bin`: `SizeOfImage` bytes in RVA order, with each byte stored at
  its RVA offset. Unreadable 4 KiB pages are zero-filled and counted in
  `module_layout.json`; this is a mapped image, not a valid on-disk PE file.
- `ghidra_import.py`: Ghidra script that imports every successfully captured
  section extent at its loaded virtual address and applies the PE section
  read/write/execute attributes. It uses `module_sections/` so unreadable gaps
  do not become fabricated code/data blocks.
- `memory_regions.json`: base/end/size, protection and memory type for every
  readable committed region included in the process scan, with module RVAs
  where a region overlaps Minecraft's image.
- `signatures.json`, `strings.json`, `paths.json`, `rtti_candidates.json`,
  `vtable_candidates.json`: bounded observations with source addresses and
  evidence notes.
- `checkpoint.json`: periodic scan cursor and process/build identity.
- `progress.json`: latest progress and skipped/read-failure counts.

The scanner walks committed readable regions discovered by `VirtualQuery`,
reads at most 64 KiB per operation through `ReadProcessMemory`, skips guard
and inaccessible pages, and yields between chunks. PE headers and the `.text`
fingerprint are also copied and hashed through guarded process-memory reads;
the scanner does not directly dereference addresses from the game. Read errors
cancel or skip the affected extent. This substantially reduces access-violation
risk, though no in-process DLL can guarantee that the host game will never
crash. Pause/resume/stop/status
commands are available in its console. `restart` stops the current pass and
starts again from the saved cursor. A checkpoint is resumable only in the same
running process, with the same executable fingerprint and matching memory
region layout; if those checks fail, it safely starts a fresh pass.

Load the DLL into the Minecraft process using the same development loader used
for the other SDK examples. A console is opened for scan progress and commands.
Commands: `pause`, `resume`, `status`, `stop`, `restart`, `exit` (`exit` closes only the
console). This tool is intended for local SDK research and does not include a
DLL injector.

## Ghidra import

Create a new empty `x86:LE:64:default` program, then run `ghidra_import.py` from
Ghidra's Script Manager and select the `windows-x64` output folder. The script
sets the image base to the address observed in the running process, imports the
captured readable extents, and labels each block with its PE section and RVA.
Ghidra can then perform its normal analysis. Keep `mapped_image.bin` alongside
the bundle as a convenient RVA-indexed reference; the importer intentionally
uses the individual extents to preserve section permissions and omit unreadable
pages.
