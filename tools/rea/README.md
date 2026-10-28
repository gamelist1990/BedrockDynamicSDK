# REA: default native analysis tools

Japanese guide: [README_ja.md](README_ja.md).

Use [REA](https://github.com/morluto/rea) with its Ghidra provider for static
analysis of the exact Minecraft Bedrock executable. REA supplies the analysis
workflow and evidence-backed queries; Ghidra performs the native disassembly
and decompilation. ScanClient remains the runtime observation tool. Compare
the two using the exact executable build and RVA.

## Prerequisites

- Node.js 22.19+ (or 24.11+ / 26+) and npm.
- Ghidra 12.1.x and a supported full JDK.
- The exact `Minecraft.Windows.exe` for the profile being researched.
- REA connected to Codex and configured with the `ghidra` provider.

This repository's tool environment has REA 6.7.0, Ghidra 12.1.4, and a
supported Node runtime. Check current readiness with:

```sh
npx --yes rea-agents@latest doctor --provider ghidra --json
npx --yes rea-agents@latest providers --json
```

For another machine, install the latest REA integration and restart Codex:

```sh
npx --yes rea-agents@latest setup --client codex
```

Review REA's displayed configuration plan before accepting it. Setup registers
REA in the Codex user configuration and installs its reverse-engineering
workflow. The project `AGENTS.md` makes REA/Ghidra the default for this repo.

## Analyze the game executable

From PowerShell:

```powershell
.\tools\rea\analyze.ps1 analyze D:\path\to\Minecraft.Windows.exe
.\tools\rea\analyze.ps1 search D:\path\to\Minecraft.Windows.exe Actor
.\tools\rea\analyze.ps1 function D:\path\to\Minecraft.Windows.exe 0x140123456
.\tools\rea\analyze.ps1 decompile D:\path\to\Minecraft.Windows.exe 0x140123456
.\tools\rea\analyze.ps1 xrefs D:\path\to\Minecraft.Windows.exe 0x140123456
```

From Linux/macOS:

```sh
tools/rea/analyze.sh analyze /path/to/Minecraft.Windows.exe
tools/rea/analyze.sh search /path/to/Minecraft.Windows.exe Actor
tools/rea/analyze.sh function /path/to/Minecraft.Windows.exe 0x140123456
tools/rea/analyze.sh decompile /path/to/Minecraft.Windows.exe 0x140123456
tools/rea/analyze.sh xrefs /path/to/Minecraft.Windows.exe 0x140123456
```

The scripts use REA's Ghidra provider, keep a reusable analysis snapshot keyed
by the target's SHA-256 prefix, and write each query's JSON evidence under
`%USERPROFILE%\Documents\PEXData\REA` or `$HOME/Documents/PEXData/REA`.
Pass an evidence directory as the fourth positional argument to the shell
script or with `-EvidenceDirectory` in PowerShell to change the destination.
Use procedure searches explicitly with the REA CLI's `search --kind
procedures` command when needed.

To port old SDK symbol candidates using both exact builds:

```sh
.venv/bin/python -m bedrock_symbols.cli port NEW-Minecraft.Windows.exe \
  --old data/1.26.52.3/windows-x64.json \
  --source-binary OLD-Minecraft.Windows.exe --version NEW_VERSION \
  --base-profile data/NEW_VERSION/windows-x64.json \
  --output /tmp/windows-x64-candidates.json
```

`--source-binary` must match the old profile's file and `.text` hashes. The
offline port derives source-unique entry/body anchors from that executable and
includes functions nested under `native_*`. Results remain candidates; it does
not transfer native layouts or ABI claims. `--base-profile` merges results
without replacing existing entries, records same-RVA matches, and stores a
different-RVA candidate under a separate alias.

## Analysis discipline

Start with `analyze`, then search for known strings/symbols, inspect a function,
and follow its callers or callees with `xrefs` and further function queries.
Keep the returned evidence JSON with notes naming the game version, executable
SHA-256, provider, address/RVA, and confidence. REA pseudocode and names remain
analysis evidence; ABI and structure layout must still be independently
verified against the exact build and runtime observations.

ScanClient captures only the Minecraft image's `.text`, `.rdata`, and `.pdata`
sections as separate readable extents. Its filtered world-transition string
references and function ranges are runtime-address evidence, not verified
symbols. For REA's native PE workflow, point it at the original
`Minecraft.Windows.exe`; use the ScanClient bundle to cross-check captured
bytes and loaded addresses. Skipped or unreadable extents are not observed
bytes.
