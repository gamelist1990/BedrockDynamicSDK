from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path

from .pe import Executable
from .signature import (find_matches, generate_signature_anchors,
                        generate_unique_signature, parse_pattern)


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
            "runtime_verified": bool(row.get("runtime_verified", False)),
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


def _port_definitions(old: dict) -> list[tuple[str, dict]]:
    """Collect flat symbols and portable native function definitions."""
    definitions: list[tuple[str, dict]] = []
    symbols = old.get("symbols", {})
    if isinstance(symbols, dict):
        definitions.extend((str(name), value) for name, value in symbols.items()
                           if isinstance(value, dict))

    def visit(node: object, path: str, depth: int = 0) -> None:
        if not isinstance(node, dict) or depth > 64 or len(definitions) >= 20_000:
            return
        anchors = node.get("signature_anchors")
        has_anchors = isinstance(anchors, list) and any(
            isinstance(anchor, dict) and anchor.get("pattern") for anchor in anchors)
        if (path and "rva" in node and
                (has_anchors or node.get("signature") or node.get("entry_check"))):
            definitions.append((path, node))
            return
        for key, value in node.items():
            visit(value, f"{path}.{key}" if path else str(key), depth + 1)

    for name, value in old.items():
        if str(name).startswith("native_"):
            visit(value, str(name))
            if len(definitions) >= 20_000:
                break
    return definitions


def _source_anchors(symbol: dict, source: Executable | None) -> list[dict[str, object]]:
    raw_anchors = symbol.get("signature_anchors", [])
    anchors = [anchor for anchor in raw_anchors
               if isinstance(anchor, dict) and anchor.get("pattern")]
    if not anchors and symbol.get("signature"):
        anchors.append({"pattern": symbol["signature"], "offset": 0,
                        "kind": "source-signature"})
    if not anchors and symbol.get("entry_check"):
        anchors.append({"pattern": symbol["entry_check"], "offset": 0,
                        "kind": "source-entry-check"})
    if source is None:
        return anchors

    old_rva_value = symbol.get("rva")
    if old_rva_value is None:
        return anchors
    try:
        old_rva = int(old_rva_value, 0) if isinstance(old_rva_value, str) else int(old_rva_value)
    except (TypeError, ValueError):
        return anchors
    if not source.text_bytes_at(old_rva, 1):
        return anchors

    # Bind the source profile address to the exact source executable before
    # deriving any new body anchors. A stale entry guard is not used as proof.
    entry_check = symbol.get("entry_check")
    if entry_check:
        try:
            code, mask = parse_pattern(str(entry_check))
        except ValueError:
            return []
        observed = source.text_bytes_at(old_rva, len(code))
        if len(observed) != len(code) or any(
                fixed and observed[index] != byte
                for index, (byte, fixed) in enumerate(zip(code, mask))):
            return []

    # Existing profile anchors are evidence only when they really occur at
    # their recorded source-function offsets in the exact source executable.
    source_anchors: list[dict[str, object]] = []
    for anchor in anchors:
        try:
            pattern = str(anchor["pattern"])
            offset = int(anchor.get("offset", 0))
            code, mask = parse_pattern(pattern)
        except (KeyError, TypeError, ValueError):
            continue
        anchor_rva = old_rva + offset
        observed = source.text_bytes_at(anchor_rva, len(code))
        if len(observed) != len(code) or any(
                fixed and observed[index] != byte
                for index, (byte, fixed) in enumerate(zip(code, mask))):
            continue
        hits = find_matches(source.text_data, pattern, max_matches=1)
        if hits == [anchor_rva - source.text_rva]:
            source_anchors.append(anchor)
    anchors = source_anchors

    function_start = source.containing_function(old_rva)
    function_end = source.function_end(function_start) if function_start is not None else None
    if function_end is not None:
        length = min(function_end - old_rva, 0x4000)
    else:
        length = 0x100
    if length < 20:
        return anchors
    code = source.text_bytes_at(old_rva, length)
    if len(code) < 20:
        return anchors
    try:
        derived = generate_signature_anchors(
            code, source.text_data, old_rva - source.text_rva)
    except ValueError:
        return anchors
    seen = {str(anchor["pattern"]) for anchor in anchors}
    anchors.extend(anchor for anchor in derived if str(anchor["pattern"]) not in seen)
    return anchors


def port_database(old: dict, exe: Executable, version: str,
                  source_exe: Executable | None = None) -> dict:
    """Port symbol candidates using exact-build entry/body anchors and new .pdata.

    All results remain candidates. Every bounded hit votes for its containing
    function; distinct anchors increase support. A tied top result fails closed.
    """
    targets = []
    warnings = []
    support: dict[str, dict[str, int]] = {}
    containing = getattr(exe, "containing_function", None)
    boundaries = set(exe.pdata_bounds()) if not callable(containing) else set()
    if source_exe is not None:
        old_target = old.get("target", {})
        expected_sha = str(old_target.get("sha256", "")).lower()
        expected_text_sha = str(old_target.get("text_sha256", "")).lower()
        if expected_sha and source_exe.sha256.lower() != expected_sha:
            raise ValueError("source executable SHA-256 does not match the source profile")
        if expected_text_sha and source_exe.text_sha256.lower() != expected_text_sha:
            raise ValueError("source executable .text SHA-256 does not match the source profile")

    definitions = _port_definitions(old)
    for name, symbol in definitions:
        anchors = _source_anchors(symbol, source_exe)
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
        rva, matched_anchors = ranked[0]
        targets.append({
            "name": name,
            "rva": rva,
            "source": f"anchor-port:{old['target']['version']} ({len(matched_anchors)}/{len(anchors)} source-unique anchors)",
            "verified": False,
        })
        support[name] = {
            "matched_anchors": len(matched_anchors),
            "available_anchors": len(anchors),
            "anchors_over_match_cap": capped_anchors,
            "candidate_functions": len(ranked),
            "rva": f"0x{rva:X}",
            "matched_anchor_indices": sorted(matched_anchors),
        }
        if len(matched_anchors) < 2 and len(anchors) > 1:
            warnings.append(f"{name}: weak candidate supported by one anchor pattern")
    generated = make_database(exe, version, old["target"].get("kind", "client"),
                              targets=targets, include_exports=False,
                              max_entries=len(targets))
    definitions_by_name = dict(definitions)
    for name, result in generated["symbols"].items():
        source = definitions_by_name.get(name, {})
        result.update({
            "source_definition_path": name,
            "source_rva": source.get("rva"),
            "source_abi": source.get("abi", ""),
            "source_status": source.get("status", "candidate"),
            "source_description": source.get("source", ""),
            "source_evidence": source.get("evidence"),
        })
    generated["generation"]["warnings"].extend(warnings)
    generated["generation"]["port_support"] = support
    generated["generation"]["source_version"] = old.get("target", {}).get("version", "unknown")
    generated["generation"]["source_executable_sha256"] = source_exe.sha256 if source_exe else None
    generated["generation"]["source_text_sha256"] = source_exe.text_sha256 if source_exe else None
    if old.get("native_items"):
        generated["generation"]["warnings"].append(
            "native_items layout was not copied: revalidate Actor equipment, local inventory, "
            "component IDs, slot indices, string fields, and native name ABI for this executable"
        )
    return generated


def merge_port_candidates(base: dict, ported: dict) -> dict:
    """Merge candidates into an exact-build profile without replacing evidence."""
    base_target = base.get("target", {})
    port_target = ported.get("target", {})
    for key in ("sha256", "text_sha256", "version"):
        if str(base_target.get(key, "")).lower() != str(port_target.get(key, "")).lower():
            raise ValueError(f"base profile and ported candidates disagree on target {key}")
    if not isinstance(base.get("symbols"), dict):
        raise ValueError("base profile has no symbols object")

    merged = json.loads(json.dumps(base))
    symbols = merged["symbols"]
    added: list[str] = []
    same_rva: list[str] = []
    alternatives: dict[str, str] = {}
    for name, candidate in ported.get("symbols", {}).items():
        current = symbols.get(name)
        if current is None:
            symbols[name] = candidate
            added.append(name)
            continue
        if str(current.get("rva", "")).lower() == str(candidate.get("rva", "")).lower():
            same_rva.append(name)
            continue
        alias = f"candidate::cross_version_port::{name}"
        suffix = 2
        while alias in symbols:
            alias = f"candidate::cross_version_port::{name}::{suffix}"
            suffix += 1
        symbols[alias] = candidate
        alternatives[name] = alias

    analysis = merged.setdefault("analysis", {})
    if not isinstance(analysis, dict):
        analysis = {}
        merged["analysis"] = analysis
    analysis["cross_version_port"] = {
        "source_version": ported.get("generation", {}).get("source_version", "unknown"),
        "source_profile": ported.get("generation", {}).get("source_profile", ""),
        "source_executable_sha256": ported.get("generation", {}).get("source_executable_sha256"),
        "source_text_sha256": ported.get("generation", {}).get("source_text_sha256"),
        "candidate_only": True,
        "native_layouts_transferred": False,
        "ported_candidates": ported.get("generation", {}).get("port_support", {}),
        "added_symbols": added,
        "same_rva_symbols": same_rva,
        "alternate_symbols": alternatives,
        "warnings": ported.get("generation", {}).get("warnings", []),
    }
    return merged


def write_json(out: Path, value: dict) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
