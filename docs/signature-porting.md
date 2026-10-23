# Cross-version signature porting

The checked-in Windows x64 profile targets one exact executable. A signature
that uniquely matches this `.text` is evidence for that build only; it does not
prove a match against another Minecraft release. The runtime resolver still
requires an exact supported image fingerprint before it treats profile RVAs as
verified. Offline port results remain candidates.

## Signature anchors

`bedrock-symbols generate` records several short, instruction-aligned anchors
from each known function. It includes the entry and spaced body locations, masks
relative branch/call operands and memory displacements, and keeps only patterns
that uniquely identify their exact source location in the source `.text`. Each
anchor records its source offset, byte/fixed-byte counts, and `unique-in-source-text`
provenance. The offset describes the source build; the destination port locates
the matching bytes inside the destination build's x64 `.pdata` function range.

`bedrock-symbols port` searches all anchors. Every hit is mapped to its containing
destination `.pdata` function and votes for that function entry; repeated hits for
one anchor inside one function count once. This lets other anchors disambiguate a
pattern that occurs in multiple functions. Patterns exceeding the bounded hit cap
are ignored with a warning, and conflicting top-ranked entries are rejected. The
selected entry is used to regenerate that build's own RVA, entry check, and signature.
Every ported result is still `candidate`, `abi_verified=false`; a byte match cannot
prove that the function retained its old meaning or ABI. Review the new function
body, callers, data/layout dependencies, and ABI in static analysis before
promoting it. Re-run the port when a new scan/EXE becomes available.

## 1.26.52.3 audit

The source executable SHA-256 is
`4a92bfa3ce2428b40ee517b7c1125d9f6f79274382de03846351992663b2e2e4`; its `.text`
SHA-256 is `6fa218d247874940235aacb8fd7e3bbf0e3d1e259630d7f83fcb536f06733aeb`.
The attached ScanClient manifest reports the same hashes and image size. The
profile's 11 named symbol patterns each match their recorded RVA uniquely in
this source `.text`. The prior `Level::getRuntimeActorList` pattern was 60 bytes;
a generated 30-byte pattern still uniquely matches its entry after masking stack
frame placement. `ItemStackBase::getHoverName` likewise has a generated unique
entry pattern, so the old profile's `cross_version_signature_available=false`
was stale. The other nine symbols already have usable unique patterns.

The audit generated anchors for each profile function. This is source-build
coverage and a same-build `.pdata` mapping control only:

| Symbol | RVA | `.pdata` size | Entry signature bytes | Port anchors |
| --- | ---: | ---: | ---: | ---: |
| `Level::getRuntimeActorList` | `0x117C640` | 159 | 30 | 3 |
| `LevelRendererPlayer::getFov` | `0x46BD250` | 763 | 29 | 5 |
| `ClientInstance::grabCursor` | `0x5DCE3C0` | 67 | 45 | 0; uses entry fallback |
| `ClientInstance::releaseCursor` | `0x5DCE410` | 67 | 45 | 0; uses entry fallback |
| `MultiPlayerLevel::_subTick` | `0x12F4B70` | 1,272 | 24 | 3 |
| `MouseDevice::feed` | `0x361AD0` | 501 | 36 | 5 |
| `GameCore::keyboardEvent` | `0x98FA0` | 205 | 32 | 3 |
| `Keyboard::feed` | `0x2D18D0` | 393 | 27 | 2 |
| `GameCore::handleMouseInput` | `0x8A880` | 11,370 | 119 | 2 body anchors |
| `Actor::getNameTag` | `0x1A14AC0` | 97 | 38 | 1 |
| `ItemStackBase::getHoverName` | `0x24901E0` | 311 | 34 | 2 |

All 11 existing primary signatures uniquely matched their recorded entry in the
source `.text`; the new anchor port path mapped all 11 back to the same `.pdata`
entry when run against this same executable. This sanity check does not measure
cross-version matching.

The current ScanClient `signatures.json` contains 1,500 generic page-level
candidates, not named function signatures. Its last listed address is around RVA
`0x5DC000`, while the `.text` spans about `0xE5A5C00`; this candidate list
therefore covers only an early slice of the image and cannot establish function
identity. The complete `mapped_image.bin` and matching executable fingerprint are
useful for exact-build byte checks, but the candidate list must not be copied
into the SDK profile as verified symbols.

The supplied data contains only build 1.26.52.3. Future-build hit rate cannot
be measured without a second executable. Body anchors and `.pdata` mapping are a
more resilient candidate locator than one long entry prefix, but optimizer
changes, inlining, function splitting, or semantic changes can still remove or
misidentify a match.
