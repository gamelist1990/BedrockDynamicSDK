"""Incomplete native profiles must not expose unverified hook targets."""
import hashlib
import json
from pathlib import Path

from bedrock_symbols.index import build_index


def test_126605_identifies_build_without_exposing_candidates():
    root = Path(__file__).resolve().parents[1] / "data"
    directory = root / "1.26.60.5"
    candidate = json.loads((directory / "windows-x64.candidate.json").read_text(encoding="utf-8"))
    raw = (directory / "windows-x64.json").read_bytes()
    profile = json.loads(raw)
    entry = build_index(root)["versions"]["1.26.60.5"]["windows-x64"]
    assert profile["target"] == candidate["target"]
    assert profile["target"]["version"] == "1.26.60.5"
    assert profile["analysis"]["status"] == "incomplete_no_runtime_symbols"
    assert profile["symbols"] == {}
    assert not any(key.startswith("native_") for key in profile)
    assert profile["analysis"]["candidate_symbols"] == candidate["symbols"]
    assert all(row["status"] == "candidate" and row["abi_verified"] is False
               for row in candidate["symbols"].values())
    assert entry["symbols"] == 0
    assert entry["sha256"] == hashlib.sha256(raw).hexdigest()
    assert entry["module_sha256"] == profile["target"]["sha256"]
    assert json.loads((root / "manifest.json").read_text())["versions"]["1.26.60.5"]["windows-x64"] == entry
