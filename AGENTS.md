# Reverse engineering workflow

For native Minecraft Bedrock executable analysis, use REA with the Ghidra
provider as the default workflow. Prefer the REA MCP tools for iterative
analysis; use `tools/rea/analyze.ps1` or `tools/rea/analyze.sh` for repeatable
CLI queries and saved JSON evidence. Do not fall back to ad-hoc Ghidra scripts
for routine analysis when REA can provide the same evidence.

Keep analysis evidence outside the repository under
`%USERPROFILE%/Documents/PEXData/REA/` on Windows, or
`$HOME/Documents/PEXData/REA/` on Linux. Use one snapshot per exact executable
and retain its SHA-256, game version, provider, and operation with findings.
Treat REA findings and decompiler output as evidence-backed hypotheses. Verify
function RVA, ABI, object layout, and runtime behavior against the exact game
build before promoting any result into the SDK's verified symbol profile.

Use ScanClient for runtime observations and REA/Ghidra for static analysis.
Correlate both by executable fingerprint, RVA, and observed values; a runtime
observation alone does not establish a function signature or ABI.

# Native SDK boundary and version updates

User requirement: Mod code must use SDK APIs for native access. Add executable
signatures/RVAs, object/field offsets, vtable slots, function ABI, guards and
version selection to the SDK and `data/<version>/windows-x64.json`; do not add
another native layout or fallback constant to `exsample/*/src`.

Use stable Minecraft function identifiers under `symbols` and refer to them
from layouts with `{"symbol":"Class::function"}`. The identifier is a maintained
SDK binding name, not a claim that the shipped EXE contains that debug symbol.
Keep one executable function definition; do not copy its RVA/guard into Mods.
Read `docs/profile-maintenance.md` for resolution order, ABI constraints and
version migration evidence. Existing legacy native code is not permission to
add more: migrate the touched native operation behind an SDK adapter.
