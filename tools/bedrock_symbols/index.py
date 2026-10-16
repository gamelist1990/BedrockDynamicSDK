"""Index immutable versioned symbol database JSON files for GitHub raw hosting."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path


def build_index(root: Path) -> dict:
    versions: dict = {}
    for path in sorted(root.glob("*/windows-x64.json")):
        data = path.read_bytes()
        db = json.loads(data)
        target = db.get("target", {})
        version = str(target.get("version", ""))
        if not version or version != path.parent.name or target.get("arch") != "x64":
            raise ValueError(f"inconsistent version directory / target: {path}")
        entry = {
            "path": path.relative_to(root.parent).as_posix(),
            "sha256": hashlib.sha256(data).hexdigest(),
            "module_sha256": target["sha256"],
            "symbols": len(db["symbols"]),
        }
        # All analysis evidence is stored in the SAME version JSON file.
        entry["module_text_sha256"] = target["text_sha256"]
        entry["size_of_image"] = target["size_of_image"]
        entry["text_rva"] = target["text_rva"]
        entry["text_size"] = target["text_size"]
        entry["rtti_types"] = db.get("analysis", {}).get("rtti", {}).get("count", 0)
        versions.setdefault(version, {})["windows-x64"] = entry
    return {"schema": 1, "versions": versions}
