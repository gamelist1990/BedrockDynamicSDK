# Schema v1 (versioned function location data)

`target.sha256` is SHA-256 of the original *on-disk plaintext PE file*, not its .text
digest. `target.text_sha256` is the raw .text section digest. Both must be tied to
the executable used for generation.

`target.image_base` is the PE preferred base; `rva` is an offset from the actual
loaded base (independent of ASLR). `text_rva` and `text_size` refer to the analyzed
PE file. The C++ runtime scans the **live** .text mapping passed at attach-time, not
the old mapping, when probing cross-version candidates.

Symbols:
- `rva`: exact build RVA, never blindly portable.
- `signature`: full-instruction x86-64 pattern; `??` wildcards rel32 branches and
  RIP-relative displacements.
- `entry_check`: exact prefix guard used for exact-hash RVA resolution.
- `status`: `verified` (trusted on exact hash only) or `candidate` (manual review).
- `function_boundary`: whether .pdata declares the RVA as a function start.
- `source`: evidence origin; not a proof of C++ ABI compatibility.

Safety:
- On hash mismatch, default C++ lookup returns **no address**.
- Candidate retrieval must be explicitly requested, and returns `verified=false`.
- Multiple matches return no result. No guessed addresses or write/patch are performed.
- No JSON data alone can guarantee argument types, virtual tables, or object layout.
- Don't publish Minecraft EXEs or proprietary IDA databases in the symbol repository.

- When the signature is empty, cross_version_signature_available=false; restricted to exact-build RVA candidate.

## Latite/client-profile provenance (2026-10-16)

The generator scans historical Latite signatures (commit 9f7463515dd298a496da918285936d78c7416aad),
retains **only unique direct function matches with x64 .pdata function boundaries**,
then generates new AOB signatures from the current PE. Historical labels are
not semantic proof, and all are `candidate` with `abi_verified: false`.
`category`, `origin_key`, `confidence`, and `reference_commit` identify provenance.

`latite-compatibility.json` includes full assessment of matching, missing, and
invalid signatures. Globals from RIP-relative LEA addresses and matches *inside*
a function aren't silently turned into callable functions.

`layout-hints-unverified.json` contains older Latite field/vtable offsets, not
validated for the newer build. It is NOT fed to the C++ Resolver.

Runtime on GDK/Proton may derive full loaded .text hash via BCrypt rather than
hashing the encrypted on-disk EXE. Both the image size and .text RVA/size must
also match, otherwise exact-RVA resolution fails closed.
