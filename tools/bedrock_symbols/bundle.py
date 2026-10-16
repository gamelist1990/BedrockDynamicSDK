"""Combine the exact-build symbol, RTTI, historic AOB and layout evidence in ONE JSON."""
from __future__ import annotations

import json
from pathlib import Path

from .pe import Executable
from .client_profile import make_client_profile
from .rtti import extract_rtti
from .layout_hints import extract_layout_hints


def build_unified_profile(exe: Executable, version: str, latite_addresses: Path,
                          latite_repo: Path, latite_commit: str) -> dict:
    profile, compatibility = make_client_profile(exe, version, latite_addresses, latite_commit)
    types = extract_rtti(exe)
    layouts = extract_layout_hints(latite_repo, latite_commit)
    profile["analysis"] = {
        "rtti": {
            "status": "compiler_type_names_only",
            "count": types["count"],
            "types": types["types"],
        },
        "latite_compatibility": compatibility,
        "layout_hints": layouts,
    }
    return profile


def combine_existing(profile_path: Path, rtti_path: Path,
                     compatibility_path: Path, layouts_path: Path) -> dict:
    profile = json.loads(profile_path.read_text(encoding="utf-8"))
    rtti = json.loads(rtti_path.read_text(encoding="utf-8"))
    comp = json.loads(compatibility_path.read_text(encoding="utf-8"))
    layouts = json.loads(layouts_path.read_text(encoding="utf-8"))
    if profile["target"]["sha256"] != rtti["target"]["sha256"]:
        raise ValueError("RTTI source does not match symbol PE hash")
    if profile["target"]["sha256"] != comp["target"]["sha256"]:
        raise ValueError("compatibility scan source does not match symbol PE hash")
    if rtti["target"]["version"] != profile["target"]["version"]:
        raise ValueError("RTTI version does not match")
    profile["analysis"] = {
        "rtti": {"status": "compiler_type_names_only", "count": rtti["count"], "types": rtti["types"]},
        "latite_compatibility": comp,
        "layout_hints": layouts,
    }
    return profile
