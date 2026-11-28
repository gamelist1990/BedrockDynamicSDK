#!/usr/bin/env python3
"""re_package.py — build a static-analysis bundle (zip) for a PE target.

The bundle is designed to be handed to an analyst (human or an AI in a
sandbox VM that has capstone / pefile / lief / unicorn available): it
contains pre-computed, plain-format analysis artifacts so work can start
without re-deriving the basics.

Two-sided design (v1.2.0):
  * target side    — the binary under analysis: identity hashes, pe.json
                     (with per-section SHA-256), strings.
  * reference side — a profiled build of the same program: full-function
                     disassembly, masked byte-pattern variants that were
                     self-checked against the reference binary, a call-site
                     (xref) map, and the class/neighbour layout. This is
                     the material a cross-version port is built from.
                     Defaults to the target itself when no separate
                     reference is given.

Bundle layout (see README.md in the zip):
  manifest.json          target/reference identity, hashes, options used
  target/<name>          the binary itself (omit with --no-binary)
  pe.json                sections (+sha256), imports, exports
  strings.txt            ASCII + UTF-16LE strings with file offsets
  symbols.json           reference symbols (name, RVA, function size)
  disasm/<symbol>.asm    full-function capstone disassembly (reference)
  patterns.json          masked pattern variants per symbol, self-checked
  callers.json           E8/E9 call-site map into each symbol (reference)
  neighbors.json         per-class RVA ordering of symbols (reference)
  analysis/profile/<v>/  accumulated SDK profile data for every version
  analysis/extra/        investigation reports / decompilations

Usage:
  re_package.py TARGET [-o OUT.zip] [--symbols-json FILE]
                [--reference-exe FILE] [--rva 0xRVA[:SIZE]]...
                [--no-binary] [--min-string N]
                [--sdk-data DIR] [--extra-dir DIR]...

--symbols-json accepts a JSON list of {"name": str, "rva": int|hex str,
"size": int (optional)} — the REFERENCE build's symbols. Without symbols,
exported functions of the reference are used instead.
"""
import argparse
import bisect
import hashlib
import json
import math
import re
import struct
import sys
import zipfile
from collections import Counter
from pathlib import Path

import pefile
from capstone import (CS_ARCH_X86, CS_GRP_CALL, CS_GRP_JUMP, CS_MODE_32,
                      CS_MODE_64, CS_OPT_DETAIL, Cs)
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

TOOL_VERSION = "1.2.0"

TEXT_SUFFIXES = {".json", ".md", ".txt", ".c", ".h", ".cpp", ".hpp", ".py",
                 ".yml", ".yaml", ".csv", ".asm"}
MAX_EXTRA_FILE = 5 * 1024 * 1024
MAX_FUNC_DISASM = 32 * 1024
PATTERN_HEADS = (32, 64, 160)


def collect_tree(root: Path, arc_prefix: str, text_only: bool):
    """Return (entries, skipped): entries = [(arcname, path)] under root."""
    entries, skipped = [], []
    for p in sorted(root.rglob("*")):
        if not p.is_file():
            continue
        if text_only and p.suffix.lower() not in TEXT_SUFFIXES:
            skipped.append(str(p.relative_to(root)))
            continue
        if p.stat().st_size > MAX_EXTRA_FILE:
            skipped.append(str(p.relative_to(root)))
            continue
        entries.append((f"{arc_prefix}/{p.relative_to(root).as_posix()}", p))
    return entries, skipped


def count_profile_symbols(version_dir: Path) -> int:
    total = 0
    for f in (version_dir / "windows-x64" / "classes").glob("*.json"):
        try:
            d = json.loads(f.read_text(encoding="utf-8"))
        except Exception:
            continue
        total += len(d.get("symbols") or {})
    return total


def entropy(data: bytes) -> float:
    if not data:
        return 0.0
    counts = Counter(data)
    n = len(data)
    return -sum((c / n) * math.log2(c / n) for c in counts.values())


def extract_strings(data: bytes, min_len: int):
    """Yield (offset, kind, text) for ASCII and UTF-16LE strings."""
    out = []
    ascii_re = rb"[\x20-\x7e]{%d,}" % min_len
    for m in re.finditer(ascii_re, data):
        out.append((m.start(), "ascii", m.group().decode("ascii")))
    utf16_re = rb"(?:[\x20-\x7e]\x00){%d,}" % min_len
    for m in re.finditer(utf16_re, data):
        out.append((m.start(), "utf16", m.group().decode("utf-16le")))
    out.sort(key=lambda t: t[0])
    return out


def sanitize(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "_", name)[:120] or "unnamed"


def parse_rva(text: str):
    rva_part, _, size_part = text.partition(":")
    rva = int(rva_part, 0)
    size = int(size_part, 0) if size_part else None
    return rva, size


def log_phase(name: str):
    try:
        for line in open("/proc/self/status"):
            if line.startswith("VmRSS:"):
                print(f"[phase] {name}: RSS {line.split()[1]} kB",
                      file=sys.stderr, flush=True)
                return
    except Exception:
        pass
    print(f"[phase] {name}", file=sys.stderr, flush=True)


def pdata_ranges(pe):
    """Sorted [(begin_rva, end_rva)] from the exception directory.

    Parsed as raw 12-byte RUNTIME_FUNCTION entries on purpose: pefile's
    object-per-entry parse of this game's ~1M entries costs ~10 GB RSS.
    """
    try:
        d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[
            pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]]
        raw = pe.get_data(d.VirtualAddress, d.Size)
    except Exception:
        return []
    out = []
    for i in range(0, len(raw) - 11, 12):
        begin, end, _unwind = struct.unpack_from("<III", raw, i)
        if end > begin:
            out.append((begin, end))
    out.sort()
    return out


def func_range(ranges, starts, rva):
    i = bisect.bisect_right(starts, rva) - 1
    if i >= 0 and ranges[i][0] == rva:
        return ranges[i]
    if i >= 0 and ranges[i][0] < rva < ranges[i][1]:
        return ranges[i]
    return None


def masked_pattern(code: bytes, va: int, md, image_base: int, image_end: int):
    """Mask version-dependent bytes in the head of a function.

    Returns (pattern_bytes, mask) using capstone encoding offsets:
    branch/call immediates, RIP-relative displacements, and immediates
    that point inside the image are masked out.
    """
    vals = bytearray(code)
    mask = bytearray(b"\x01" * len(code))
    for insn in md.disasm(code, va):
        enc = getattr(insn, "encoding", None)
        base = insn.address - va
        is_branch = insn.group(CS_GRP_CALL) or insn.group(CS_GRP_JUMP)
        for op in insn.operands:
            if op.type == X86_OP_IMM:
                v = op.imm
                if is_branch or image_base <= v < image_end:
                    if enc is not None and enc.imm_size:
                        off = base + enc.imm_offset
                        for i in range(off, min(off + enc.imm_size, len(mask))):
                            mask[i] = 0
                    elif is_branch and len(insn.bytes) >= 2:
                        # fallback: immediates sit at the tail
                        n = 4 if len(insn.bytes) >= 5 else 1
                        for i in range(base + len(insn.bytes) - n, base + len(insn.bytes)):
                            if 0 <= i < len(mask):
                                mask[i] = 0
            elif op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                if enc is not None and enc.disp_size:
                    off = base + enc.disp_offset
                    for i in range(off, min(off + enc.disp_size, len(mask))):
                        mask[i] = 0
    return bytes(vals), bytes(mask)


def pattern_text(vals: bytes, mask: bytes) -> str:
    return " ".join(f"{b:02X}" if m else "??" for b, m in zip(vals, mask))


def masked_match(data: bytes, off: int, vals: bytes, mask: bytes) -> bool:
    if off < 0 or off + len(vals) > len(data):
        return False
    for i in range(len(vals)):
        if mask[i] and data[off + i] != vals[i]:
            return False
    return True


def count_matches(text: bytes, vals: bytes, mask: bytes, cap: int = 21) -> int:
    """Count masked-pattern matches in text (anchor on longest literal run)."""
    best_start, best_len = 0, 0
    i = 0
    while i < len(vals):
        if mask[i]:
            j = i
            while j < len(vals) and mask[j]:
                j += 1
            if j - i > best_len:
                best_start, best_len = i, j - i
            i = j
        else:
            i += 1
    if best_len < 3:
        return -1
    anchor = vals[best_start:best_start + best_len]
    count, pos = 0, 0
    while True:
        k = text.find(anchor, pos)
        if k < 0:
            return count
        if masked_match(text, k - best_start, vals, mask):
            count += 1
            if count >= cap:
                return count
        pos = k + 1


def scan_callers(text: bytes, text_rva: int, targets: set, ranges, starts):
    """One pass over .text: map each target RVA to its E8/E9 call sites."""
    callers = {t: [] for t in targets}
    counts = Counter()
    for opcode in (b"\xe8", b"\xe9"):
        pos = 0
        while True:
            k = text.find(opcode, pos)
            if k < 0 or k + 5 > len(text):
                break
            rel = struct.unpack_from("<i", text, k + 1)[0]
            tgt = text_rva + k + 5 + rel
            if tgt in targets:
                counts[tgt] += 1
                if len(callers[tgt]) < 50:
                    fr = func_range(ranges, starts, text_rva + k)
                    callers[tgt].append({
                        "call_rva": text_rva + k,
                        "kind": "call" if opcode == b"\xe8" else "jmp",
                        "in_function": fr[0] if fr else None,
                    })
            pos = k + 1
    return callers, counts


def main() -> int:
    ap = argparse.ArgumentParser(description="Build a static-analysis zip bundle for a PE file.")
    ap.add_argument("target", type=Path)
    ap.add_argument("-o", "--output", type=Path, default=None)
    ap.add_argument("--symbols-json", type=Path, default=None,
                    help='JSON list of {"name","rva","size"?} for the REFERENCE build')
    ap.add_argument("--reference-exe", type=Path, default=None,
                    help="profiled reference build (default: the target itself)")
    ap.add_argument("--rva", action="append", default=[],
                    help="extra RVA[:SIZE] to disassemble (repeatable)")
    ap.add_argument("--no-binary", action="store_true",
                    help="do not embed the target binary in the zip")
    ap.add_argument("--disasm-bytes", type=int, default=512,
                    help="fallback disassembly window when no .pdata range exists")
    ap.add_argument("--min-string", type=int, default=5)
    ap.add_argument("--sdk-data", type=Path, default=None,
                    help="BedrockDynamicSDK data/ dir: every version profile "
                         "is included under analysis/profile/ for diffing")
    ap.add_argument("--extra-dir", type=Path, action="append", default=[],
                    help="extra analysis dir (reports/decompilations), "
                         "included under analysis/extra/<name>/ (repeatable)")
    args = ap.parse_args()

    target: Path = args.target
    if not target.is_file():
        print(f"target not found: {target}", file=sys.stderr)
        return 2
    data = target.read_bytes()
    sha256 = hashlib.sha256(data).hexdigest()

    ref_path: Path = args.reference_exe or target
    ref_data = ref_path.read_bytes() if ref_path != target else data
    ref_sha256 = hashlib.sha256(ref_data).hexdigest()
    same_build = ref_sha256 == sha256

    pe = pefile.PE(data=data, fast_load=True)
    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
    image_base = pe.OPTIONAL_HEADER.ImageBase
    is_64 = pe.OPTIONAL_HEADER.Magic == 0x20B
    ref_pe = pe if same_build else pefile.PE(data=ref_data, fast_load=True)
    ref_base = ref_pe.OPTIONAL_HEADER.ImageBase
    ref_end = ref_base + ref_pe.OPTIONAL_HEADER.SizeOfImage

    log_phase("loaded")
    # ---- pe.json (target) ---
    sections = []
    text_sec = None
    for s in pe.sections:
        raw = s.get_data()
        name = s.Name.rstrip(b"\x00").decode("ascii", "replace")
        if name == ".text":
            text_sec = s
        sections.append({
            "name": name,
            "rva": s.VirtualAddress,
            "virtual_size": s.Misc_VirtualSize,
            "raw_offset": s.PointerToRawData,
            "raw_size": s.SizeOfRawData,
            "sha256": hashlib.sha256(raw).hexdigest(),
            "entropy": round(entropy(raw), 3),
            "characteristics": f"0x{s.Characteristics:08x}",
        })
    imports = {}
    if hasattr(pe, "DIRECTORY_ENTRY_IMPORT"):
        for entry in pe.DIRECTORY_ENTRY_IMPORT:
            dll = entry.dll.decode("ascii", "replace")
            imports[dll] = [
                (imp.name.decode("ascii", "replace") if imp.name else f"ord{imp.ordinal}")
                for imp in entry.imports
            ]
    exports = []
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        for sym in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            exports.append({
                "name": sym.name.decode("ascii", "replace") if sym.name else None,
                "rva": sym.address,
                "ordinal": sym.ordinal,
            })
    pe_info = {
        "machine": f"0x{pe.FILE_HEADER.Machine:04x}",
        "is_64bit": is_64,
        "image_base": image_base,
        "entry_point_rva": pe.OPTIONAL_HEADER.AddressOfEntryPoint,
        "size_of_image": pe.OPTIONAL_HEADER.SizeOfImage,
        "timestamp": pe.FILE_HEADER.TimeDateStamp,
        "sections": sections,
        "imports": imports,
        "exports": exports,
    }

    log_phase("pe.json done")
    # ---- reference symbols ---
    symbols = []
    if args.symbols_json:
        raw = json.loads(args.symbols_json.read_text(encoding="utf-8"))
        for item in raw:
            rva = item["rva"]
            if isinstance(rva, str):
                rva = int(rva, 0)
            symbols.append({"name": item["name"], "rva": rva,
                            "size": item.get("size")})
    for spec in args.rva:
        rva, size = parse_rva(spec)
        symbols.append({"name": f"rva_{rva:x}", "rva": rva, "size": size})
    if not symbols:
        symbols = [{"name": e["name"] or f"ord{e['ordinal']}", "rva": e["rva"],
                    "size": None} for e in exports]

    # ---- reference-side derived data ------------------------------------
    ranges = pdata_ranges(ref_pe)
    starts = [r[0] for r in ranges]
    ref_text_sec = next(
        (s for s in ref_pe.sections if s.Name.rstrip(b"\x00") == b".text"), None)
    ref_text = ref_text_sec.get_data() if ref_text_sec is not None else b""
    ref_text_rva = ref_text_sec.VirtualAddress if ref_text_sec is not None else 0

    md = Cs(CS_ARCH_X86, CS_MODE_64 if is_64 else CS_MODE_32)
    md_detail = Cs(CS_ARCH_X86, CS_MODE_64 if is_64 else CS_MODE_32)
    md_detail.detail = True

    disasm_files = {}
    patterns = {}
    for sym in symbols:
        rva = sym["rva"]
        try:
            offset = ref_pe.get_offset_from_rva(rva)
        except Exception:
            continue
        fr = func_range(ranges, starts, rva)
        if fr and fr[0] == rva:
            sym["size"] = fr[1] - fr[0]
            window = min(fr[1] - fr[0], MAX_FUNC_DISASM)
        else:
            window = sym.get("size") or args.disasm_bytes
        code = ref_data[offset:offset + window]
        lines = [f"; {sym['name']}",
                 f"; RVA 0x{rva:x}  VA 0x{ref_base + rva:x}",
                 f"; file offset 0x{offset:x}  function bytes {len(code)}"
                 + ("" if fr else "  (no .pdata range; window fallback)"), ""]
        for insn in md.disasm(code, ref_base + rva):
            lines.append(f"0x{insn.address:x}:\t{insn.mnemonic}\t{insn.op_str}")
        disasm_files[f"disasm/{sanitize(sym['name'])}.asm"] = "\n".join(lines) + "\n"

        # masked pattern variants from the function head
        head = ref_data[offset:offset + max(PATTERN_HEADS)]
        variants = {}
        for n in PATTERN_HEADS:
            if n > len(head):
                continue
            vals, mask = masked_pattern(head[:n], ref_base + rva, md_detail,
                                        ref_base, ref_end)
            off_in_text = rva - ref_text_rva
            variants[f"entry{n}"] = {
                "pattern": pattern_text(vals, mask),
                "literal_bytes": int(sum(mask)),
                "self_match": masked_match(ref_text, off_in_text, vals, mask),
                "reference_matches": count_matches(ref_text, vals, mask),
            }
        patterns[sym["name"]] = {"rva": rva, "variants": variants}

    log_phase("disasm+patterns done")
    # call-site map over the reference .text
    callers_map, caller_counts = scan_callers(
        ref_text, ref_text_rva, {s["rva"] for s in symbols}, ranges, starts)
    callers = {}
    for sym in symbols:
        lst = callers_map.get(sym["rva"], [])
        callers[sym["name"]] = {
            "rva": sym["rva"],
            "call_count": int(caller_counts.get(sym["rva"], 0)),
            "callers": lst,
        }

    # per-class neighbour layout
    groups = {}
    for sym in symbols:
        cls = sym["name"].split("::")[0] if "::" in sym["name"] else "_global"
        groups.setdefault(cls, []).append(sym)
    neighbors = {}
    for cls, lst in sorted(groups.items()):
        lst.sort(key=lambda s: s["rva"])
        neighbors[cls] = [
            {"name": s["name"], "rva": s["rva"],
             "delta_from_prev": (s["rva"] - lst[i - 1]["rva"]) if i else None}
            for i, s in enumerate(lst)
        ]

    log_phase("callers+neighbors done")
    # ---- strings (target) ---
    strings = extract_strings(data, args.min_string)
    strings_txt = "".join(f"{off:08x} {kind:5s} {text}\n" for off, kind, text in strings)

    log_phase("strings done")
    # ---- accumulated analysis data ---
    analysis_entries = []
    analysis_skipped = []
    versions_summary = {}
    if args.sdk_data and args.sdk_data.is_dir():
        for ver_dir in sorted(args.sdk_data.iterdir()):
            if ver_dir.is_dir() and (ver_dir / "windows-x64").is_dir():
                entries, skipped = collect_tree(
                    ver_dir, f"analysis/profile/{ver_dir.name}", text_only=False)
                analysis_entries += entries
                analysis_skipped += skipped
                versions_summary[ver_dir.name] = {
                    "symbols": count_profile_symbols(ver_dir),
                    "files": len(entries),
                }
        idx = args.sdk_data / "manifest.json"
        if idx.is_file():
            analysis_entries.append(("analysis/profile/manifest.json", idx))
    for extra in args.extra_dir:
        if extra.is_dir():
            entries, skipped = collect_tree(
                extra, f"analysis/extra/{extra.name}", text_only=True)
            analysis_entries += entries
            analysis_skipped += [f"{extra.name}/{s}" for s in skipped]

    manifest = {
        "tool": "re_package",
        "tool_version": TOOL_VERSION,
        "target_name": target.name,
        "target_size": len(data),
        "target_sha256": sha256,
        "image_base": image_base,
        "reference_name": ref_path.name,
        "reference_sha256": ref_sha256,
        "reference_is_target": same_build,
        "binary_included": not args.no_binary,
        "symbols_disassembled": len(disasm_files),
        "strings_extracted": len(strings),
        "analysis_versions": versions_summary,
        "analysis_files": len(analysis_entries),
        "analysis_skipped": analysis_skipped,
    }

    readme = (
        "# Static-analysis bundle — cross-version signature porting kit\n\n"
        f"Target: `{target.name}` (file SHA-256 `{sha256}`)\n"
        f"Reference build: `{ref_path.name}` (file SHA-256 `{ref_sha256}`)"
        + (" — same build as target.\n\n" if same_build else
           " — the profiled build the reference data describes.\n\n")
        + "## Hash conventions (read this before comparing anything)\n"
        "- File SHA-256: the raw bytes of the .exe file (`manifest.json`).\n"
        "- Section SHA-256: `pe.json` hashes each section's RAW bytes "
        "(SizeOfRawData). The SDK profile's `text_sha256` uses this same "
        "raw convention — do not truncate to VirtualSize.\n"
        "- The SDK profile `target.sha256` is an image-normalized hash "
        "maintained by the SDK tooling; it is NOT the file hash and cannot "
        "be reproduced from the file alone. Leave it to the SDK side; "
        "never copy a reference value into a target manifest.\n\n"
        "## Layout\n"
        "- `pe.json` — target sections (with sha256), imports, exports\n"
        "- `strings.txt` — target strings, `offset kind text`\n"
        "- `symbols.json` — the port targets: the reference profile's "
        "headline symbols (name, RVA, function size). ONLY these are port "
        "targets; internal fields inside profile class files are not.\n"
        "- `disasm/<symbol>.asm` — FULL function disassembly from the "
        "reference build (.pdata range), VA addresses\n"
        "- `patterns.json` — per symbol, masked byte-pattern variants "
        "(entry32/64/head). `??` marks version-dependent bytes (branch "
        "displacements, RIP-relative displacements, in-image pointers) "
        "already masked for you. `self_match` and `reference_matches` were "
        "computed against the reference binary: prefer variants with "
        "reference_matches == 1.\n"
        "- `callers.json` — every E8/E9 call site in the reference that "
        "targets the symbol, with the containing function. Use these to "
        "disambiguate when a pattern matches several places in the "
        "target: the right candidate is the one whose callers mirror "
        "these.\n"
        "- `neighbors.json` — symbols grouped by class, ordered by RVA. "
        "Functions of a class cluster; a candidate far outside the "
        "cluster of its already-ported neighbours is suspect.\n"
        "- `analysis/profile/<version>/` — the SDK profile trees for "
        "every known version; the reference tree is the format template.\n\n"
        "## Porting procedure (per symbol, in order)\n"
        "1. Scan the target .text for the longest pattern variant. "
        "Unique hit + target .pdata function start at the same RVA = "
        "ported, strong evidence.\n"
        "2. Multiple hits: extend with version-stable bytes from "
        "`disasm/` (mask displacements the same way), or disambiguate "
        "via `callers.json` (find the reference callers' counterparts "
        "in the target and see which candidate they call).\n"
        "3. No hit: the function changed; say so. Do not force a match.\n"
        "4. Record per symbol: status (ported / ambiguous / not_found), "
        "the RVA or null, and one line of evidence (pattern length, "
        "match count, .pdata alignment, xref basis).\n\n"
        "## Output\n"
        "Mirror the reference profile tree for the target version "
        "(same class files and symbol names). Ported symbols keep the "
        "reference signature unless the target bytes differ (then say "
        "so in evidence); set status `candidate`, `runtime_verified` "
        "false. Unresolved symbols keep their entry with `rva: null` "
        "and the blocker in evidence. In the target manifest, fill "
        "version/file_version, size_of_image, text_rva, text_size "
        "(VirtualSize) and text_sha256 (raw section hash, from pe.json) "
        "from the TARGET; leave `target.sha256` out entirely — the SDK "
        "side computes it.\n"
    )

    out = args.output or target.with_suffix(target.suffix + ".repack.zip")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        z.writestr("manifest.json", json.dumps(manifest, indent=2))
        z.writestr("README.md", readme)
        if not args.no_binary:
            z.write(target, f"target/{target.name}")
        z.writestr("pe.json", json.dumps(pe_info, indent=2))
        z.writestr("strings.txt", strings_txt)
        z.writestr("symbols.json", json.dumps(symbols, indent=2))
        z.writestr("patterns.json", json.dumps(patterns, indent=2))
        z.writestr("callers.json", json.dumps(callers, indent=2))
        z.writestr("neighbors.json", json.dumps(neighbors, indent=2))
        for name, text in sorted(disasm_files.items()):
            z.writestr(name, text)
        for arcname, path in analysis_entries:
            z.write(path, arcname)

    print(f"wrote {out} ({out.stat().st_size} bytes): "
          f"{len(disasm_files)} disasm, {len(strings)} strings, "
          f"{len(sections)} sections, {sum(len(v) for v in imports.values())} imports")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
