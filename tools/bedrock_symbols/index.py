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
        types_path = path.parent / "types-windows-x64.json"
        if types_path.is_file():
            types_bytes = types_path.read_bytes()
            type_data = json.loads(types_bytes)
            if (type_data.get("target", {}).get("version") != version or
                    type_data["target"]["sha256"] != target["sha256"]):
                raise ValueError(f"RTTI type database references different binary: {types_path}")
            entry["rtti"] = {
                "path": types_path.relative_to(root.parent).as_posix(),
                "sha256": hashlib.sha256(types_bytes).hexdigest(),
                "type_count": len(type_data["types"]),
            }
        versions.setdefault(version, {})["windows-x64"] = entry
    return {"schema": 1, "versions": versions}
