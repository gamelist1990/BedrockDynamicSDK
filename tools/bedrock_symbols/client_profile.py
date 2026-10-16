"""Produce a feature-oriented standalone Bedrock client SDK symbol profile.

References are semantic hints only: the source Latite signature is verified as an
exact byte match for this EXE, *not* as a validated C++ signature/ABI.
"""
from __future__ import annotations

from pathlib import Path

from .generator import make_database
from .latite import scan_latite
from .pe import Executable

# Prioritize framework features rather than optional game-specific cheats/mods.
_FEATURES = {
    "core": ("ClientInstance", "MinecraftGame", "GameCore", "Platform_GameCore",
             "MainWindow", "AppPlatform"),
    "input": ("ClientInputUpdateSystem", "LocalPlayer_applyTurnDelta",
              "mouseDevice", "handleMouseInput", "Cursor"),
    "ui": ("Screen", "GuiData", "Options_", "Chat", "GuiMessage", "I18n"),
    "render": ("Renderer", "render", "Tessellator", "GpuInfo", "Material", "Mesh"),
    "world": ("Actor_", "Level", "Dimension", "MultiPlayer", "_updatePlayer"),
    "network": ("Packet", "RakPeer"),
}

def feature_for(name: str) -> str:
    for category, hints in _FEATURES.items():
        if any(hint.lower() in name.lower() for hint in hints):
            return category
    return "misc"


def make_client_profile(exe: Executable, version: str, latite_addresses: Path,
                        commit: str = "9f74635") -> tuple[dict, dict]:
    compatibility = scan_latite(latite_addresses, exe)
    targets = []
    rejected = []
    labels = set()
    for e in compatibility["entries"]:
        # Exclude addresses outside executable instructions, including RTTI globals.
        if e["status"] != "candidate_pattern_hit" or not e["function_boundary"]:
            rejected.append(e["key"])
            continue
        rva = int(e["resolved_candidate_rva"], 16)
        if not exe.text_bytes_at(rva, 1):
            rejected.append(e["key"])
            continue
        # Human-readable C++ semantic label supplied by Latite.
        name = e["description"]
        if name in labels:
            name = e["key"]
        labels.add(name)
        targets.append({
            "name": name,
            "rva": rva,
            "source": f"LatiteClient/Latite@{commit}:src/mc/Addresses.h",
            "origin_key": e["key"],
            "reference_commit": commit,
            "category": feature_for(e["key"]),
            "confidence": "unique-AOB-and-pdata-boundary",
            "source_text_sha256": exe.text_sha256,
            "verified": False,
            "abi_verified": False,
        })
    symbols = make_database(exe, version, "client", targets,
                            include_exports=False, max_entries=len(targets))
    symbols["generation"]["input_reference"] = "LatiteClient/Latite, src/mc/Addresses.h"
    symbols["generation"]["historical_pattern_matches"] = compatibility["matched"]
    symbols["generation"]["rejected_matching_or_unmapped_entries"] = rejected
    symbols["generation"]["note"] = (
        "Names come from legacy Latite SDK, checked by unique byte patterns and "
        "x64 pdata boundaries; function behavior, parameters and offsets are not ABI-verified."
    )
    return symbols, compatibility
