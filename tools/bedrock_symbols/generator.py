from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path

from .pe import Executable
from .signature import find_matches, generate_signature_anchors, generate_unique_signature


def make_database(exe: Executable, version: str, role: str = "client",
                  targets: list[dict] | None = None,
                  include_exports: bool = True, max_entries: int = 50) -> dict:
    """Generate exactly matched, file-bound data. Never label unproven mappings verified."""
    if role not in ("client", "server"):
        raise ValueError("role must be client or server")
    rows = []
    if include_exports:
        rows.extend({"name": name, "rva": exe.follow_export_thunk(rva),
                     "source": "pe-export", "verified": True}
                    for name, rva in exe.exports())
    rows.extend(targets or [])
    bounds = exe.pdata_bounds()
    results = {}
    errors = []
    for row in rows[:max_entries]:
        name = str(row["name"])
        rva = int(row["rva"], 0) if isinstance(row["rva"], str) else int(row["rva"])
        if not name or name in results:
            continue
        if not exe.text_bytes_at(rva, 1):
            errors.append(f"{name}: rva 0x{rva:x} is not within .text")
            continue
        # Known evidence against a particular version is never silently transferred.
        required_hash = str(row.get("source_text_sha256", "")).lower()
        if required_hash and required_hash != exe.text_sha256:
            errors.append(f"{name}: reference .text SHA-256 does not match")
            continue
        function_end = exe.function_end(rva)
        max_bytes = min(0x10000, function_end - rva) if function_end else 0x1400
        snippet = exe.text_bytes_at(rva, max_bytes)
        result = generate_unique_signature(snippet, exe.text_data, rva - exe.text_rva, max_length=256)
        if result is None:
            # Keep the exact-build RVA even when a portable signature cannot be identified.
            errors.append(f"{name}: no unique signature <= 256 bytes; exact-RVA candidate only")
            pattern, pattern_length = "", 0
        else:
            pattern, pattern_length = result
        anchors = (generate_signature_anchors(snippet, exe.text_data, rva - exe.text_rva)
                   if function_end else [])
        results[name] = {
            "rva": f"0x{rva:X}",
            "signature": pattern,
            "entry_check": snippet[:min(12, len(snippet))].hex(" ").upper(),
            "cross_version_signature_available": bool(pattern),
            "status": "verified" if row.get("verified", False) else "candidate",
            "source": str(row.get("source", "manual-rva")),
            "function_boundary": rva in bounds,
            "signature_length": pattern_length,
            "signature_anchors": anchors,
            "category": str(row.get("category", "unknown")),
            "origin_key": str(row.get("origin_key", "")),
            "reference_commit": str(row.get("reference_commit", "")),
            "confidence": str(row.get("confidence", "candidate")),
            "abi_verified": bool(row.get("abi_verified", False)),
        }
    return {
        "schema": 1,
        "target": {
            "product": "minecraft-bedrock",
            "kind": role,
            "platform": "windows",
            "arch": "x64",
            "version": version,
            "sha256": exe.sha256,
            "text_sha256": exe.text_sha256,
            "image_base": f"0x{exe.image_base:X}",
            "size_of_image": exe.image_size,
            "text_rva": f"0x{exe.text_rva:X}",
            "text_size": exe.text_size,
        },
        "symbols": results,
        "generation": {
            "generator": "BedrockDynamicSDK",
            "source": exe.path.name,
            "generated_utc": datetime.now(timezone.utc).isoformat(),
            "count": len(results),
            "warnings": errors,
        }
    }


def port_database(old: dict, exe: Executable, version: str) -> dict:
    """Port symbol candidates using exact-build entry/body anchors and new .pdata.

    All results remain candidates. Every bounded hit votes for its containing
    function; distinct anchors increase support. A tied top result fails closed.
    """
    targets = []
    warnings = []
    support: dict[str, dict[str, int]] = {}
    containing = getattr(exe, "containing_function", None)
    boundaries = set(exe.pdata_bounds()) if not callable(containing) else set()
    for name, symbol in old.get("symbols", {}).items():
        anchors = list(symbol.get("signature_anchors", []))
        if not anchors and symbol.get("signature"):
            anchors = [{"pattern": symbol["signature"], "offset": 0}]
        votes: dict[int, set[int]] = {}
        capped_anchors = 0
        for index, anchor in enumerate(anchors):
            pattern = anchor.get("pattern", "")
            if not pattern:
                continue
            offsets = find_matches(exe.text_data, pattern)
            if offsets is None:
                capped_anchors += 1
                warnings.append(f"{name}: anchor {index} exceeded the match cap; ignored")
                continue
            for offset in offsets:
                matched_rva = exe.text_rva + offset
                function_rva = (containing(matched_rva) if callable(containing)
                                else (matched_rva if matched_rva in boundaries else None))
                if function_rva is None:
                    continue
                votes.setdefault(function_rva, set()).add(index)
        if not votes:
            warnings.append(f"{name}: no anchor hit mapped to a .pdata function")
            continue
        ranked = sorted(votes.items(), key=lambda row: (-len(row[1]), row[0]))
        top_score = len(ranked[0][1])
        if len(ranked) > 1 and len(ranked[1][1]) == top_score:
            warnings.append(f"{name}: conflicting .pdata function candidates ({top_score} anchors each)")
            continue
        rva, matched = ranked[0]
        targets.append({
            "name": name,
            "rva": rva,
            "source": f"anchor-port:{old['target']['version']} ({len(matched)}/{len(anchors)} source-unique anchors)",
            "verified": False,
        })
        support[name] = {
            "matched_anchors": len(matched),
            "available_anchors": len(anchors),
            "anchors_over_match_cap": capped_anchors,
        }
        if len(matched) < 2 and len(anchors) > 1:
            warnings.append(f"{name}: weak candidate supported by one anchor pattern")
    generated = make_database(exe, version, old["target"].get("kind", "client"),
                              targets=targets, include_exports=False,
                              max_entries=len(targets))
    generated["generation"]["warnings"].extend(warnings)
    generated["generation"]["port_support"] = support
    if old.get("native_items"):
        generated["generation"]["warnings"].append(
            "native_items layout was not copied: revalidate Actor equipment, local inventory, "
            "component IDs, slot indices, string fields, and native name ABI for this executable"
        )
    return generated


def write_json(out: Path, value: dict) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
