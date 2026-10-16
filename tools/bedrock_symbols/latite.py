"""Compatibility analysis: inspect published Latite signatures against another Bedrock PE.

Historical patterns are never labelled verified just because their bytes match.
"""
from __future__ import annotations
from pathlib import Path
import re
import struct

from .signature import parse_pattern, unique_match
from .pe import Executable

_SIG_DECL = re.compile(r"inline\s+static\s+SigImpl\s+(\w+)\s*\{([\s\S]*?)\};")
_SIG_BYTES = re.compile(r'"([0-9a-fA-F? \r\n\t]+)"_sig')
_DESCRIPTION = re.compile(r'"([^"\r\n]+)"\s*\}?\s*$', re.M)

KEYWORDS = (
    "ClientInstance", "MainWindow", "MinecraftGame", "ClientInput",
    "Keyboard", "GameCore", "Mouse", "ScreenView", "GuiData", "Options",
    "LocalPlayer", "LevelRenderer", "ChatScreen", "Actor_", "ItemStack",
    "Text", "Render", "Tessellator", "Hud"
)


def parse_latite_addresses(path: Path) -> list[dict]:
    source = path.read_text(encoding="utf-8-sig")
    results = []
    for match in _SIG_DECL.finditer(source):
        key, body = match.groups()
        pattern = _SIG_BYTES.search(body)
        if pattern is None:
            continue
        raw = " ".join(pattern.group(1).split())
        try:
            parse_pattern(raw)
        except ValueError:
            continue
        names = _DESCRIPTION.findall(body[pattern.end():])
        # Description is user-authored, not proof of symbol identity.
        label = names[-1] if names else key
        mode = "rip-rel" if "store.deref" in body or "storage.deref" in body else "direct"
        deref = re.search(r"(?:store|storage)\.deref\((\d+)\)", body)
        results.append({
            "key": key,
            "description": label,
            "legacy_pattern": raw,
            "address_mode": mode,
            "deref_offset": int(deref.group(1)) if deref else None,
            "priority": ("high" if any(k.lower() in key.lower() for k in KEYWORDS[:9])
                         else "medium" if any(k.lower() in key.lower() for k in KEYWORDS)
                         else "low"),
        })
    return results


def scan_latite(path: Path, binary: Executable) -> dict:
    entries = parse_latite_addresses(path)
    found = []
    bounds = binary.pdata_bounds()
    for row in entries:
        match = unique_match(binary.text_data, row["legacy_pattern"])
        status = "not_found_or_ambiguous"
        rva = None
        candidate_rva = None
        if match is not None:
            rva = binary.text_rva + match
            candidate_rva = rva
            status = "candidate_pattern_hit"
            if row["deref_offset"] is not None:
                pos = row["deref_offset"]
                code = binary.text_data[match + pos:match + pos + 4]
                if len(code) == 4:
                    candidate_rva = rva + pos + 4 + struct.unpack("<i", code)[0]
                    if not binary.text_bytes_at(candidate_rva, 1):
                        status = "candidate_bad_relative_target"
                else:
                    status = "candidate_bad_relative_target"
        result = {key: val for key, val in row.items() if key != "legacy_pattern"}
        result.update({"status": status,
                       "matched_instruction_rva": f"0x{rva:X}" if rva is not None else None,
                       "resolved_candidate_rva": f"0x{candidate_rva:X}" if candidate_rva is not None else None,
                       "function_boundary": candidate_rva in bounds if candidate_rva is not None else False,
                       "legacy_pattern_length": len(row["legacy_pattern"].split())})
        found.append(result)
    return {"source": "LatiteClient/Latite src/mc/Addresses.h",
            "target": {"sha256": binary.sha256, "text_sha256": binary.text_sha256,
                       "arch": "x64", "platform": "windows"},
            "status": "automated historical AOB comparison only; never a verified ABI",
            "total": len(found),
            "matched": sum(r["status"] == "candidate_pattern_hit" for r in found),
            "entries": found}
