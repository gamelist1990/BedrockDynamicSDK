from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path

from .pe import Executable
from .signature import generate_unique_signature, unique_match


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
        max_bytes = min(320, function_end - rva) if function_end else 320
        snippet = exe.text_bytes_at(rva, max_bytes)
        result = generate_unique_signature(snippet, exe.text_data, rva - exe.text_rva, max_length=256)
        if result is None:
            # Keep the exact-build RVA even when a portable signature cannot be identified.
            errors.append(f"{name}: no unique signature <= 256 bytes; exact-RVA candidate only")
            pattern, pattern_length = "", 0
        else:
            pattern, pattern_length = result
        results[name] = {
            "rva": f"0x{rva:X}",
            "signature": pattern,
            "entry_check": snippet[:min(12, len(snippet))].hex(" ").upper(),
            "cross_version_signature_available": bool(pattern),
            "status": "verified" if row.get("verified", False) else "candidate",
            "source": str(row.get("source", "manual-rva")),
            "function_boundary": rva in bounds,
            "signature_length": pattern_length,
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
    """Match old masked patterns on new .text, always retain 'candidate' status."""
    targets = []
    warnings = []
    for name, symbol in old.get("symbols", {}).items():
        pattern = symbol.get("signature")
        if not pattern:
            continue
        offset = unique_match(exe.text_data, pattern)
        if offset is None:
            warnings.append(f"{name}: zero or multiple matches")
        else:
            targets.append({
                "name": name,
                "rva": exe.text_rva + offset,
                "source": f"pattern-port:{old['target']['version']}",
                "verified": False,
            })
    generated = make_database(exe, version, old["target"].get("kind", "client"),
                              targets=targets, include_exports=False,
                              max_entries=len(targets))
    generated["generation"]["warnings"].extend(warnings)
    if old.get("native_items"):
        generated["generation"]["warnings"].append(
            "native_items layout was not copied: revalidate Actor equipment, local inventory, "
            "component IDs, slot indices, string fields, and native name ABI for this executable"
        )
    return generated


def write_json(out: Path, value: dict) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
