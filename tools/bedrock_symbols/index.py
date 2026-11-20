"""Index immutable versioned symbol database JSON files for GitHub raw hosting."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
from .profile_parts import load_profile


def build_index(root: Path) -> dict:
    existing = root / "manifest.json"
    previous = json.loads(existing.read_text(encoding="utf-8")) if existing.exists() else {}
    versions: dict = {}
    paths={p.parent.name:p for p in root.glob("*/windows-x64.json")}
    paths.update({p.parent.parent.name:p for p in root.glob("*/windows-x64/manifest.json")})
    for version_name,path in sorted(paths.items()):
        data = path.read_bytes()
        db = load_profile(path)
        target = db.get("target", {})
        version = str(target.get("version", ""))
        if not version or version != version_name or target.get("arch") != "x64":
            raise ValueError(f"inconsistent version directory / target: {path}")
        entry = {
            "path": path.relative_to(root.parent).as_posix(),
            "sha256": hashlib.sha256(data).hexdigest(),
            "module_sha256": target["sha256"],
            "symbols": len(db["symbols"]),
        }
        if "protocol" in db: entry["protocol"] = db["protocol"]
        # Vanilla IDs track an upstream data release, not the binary profile;
        # preserve the explicit game build -> data release mapping on reindex.
        old_entry = previous.get("versions", {}).get(version, {}).get("windows-x64", {})
        if "vanilla_data" in old_entry:
            entry["vanilla_data"] = old_entry["vanilla_data"]
        # Each class/analysis file is authenticated through the profile manifest.
        entry["module_text_sha256"] = target["text_sha256"]
        entry["size_of_image"] = target["size_of_image"]
        entry["text_rva"] = target["text_rva"]
        entry["text_size"] = target["text_size"]
        entry["rtti_types"] = db.get("analysis", {}).get("rtti", {}).get("count", 0)
        versions.setdefault(version, {})["windows-x64"] = entry
    default = previous.get("default_version")
    result={"schema": 1, "versions": versions}
    if default:result["default_version"]=default
    return result
