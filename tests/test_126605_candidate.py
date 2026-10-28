"""Incomplete native profiles must not expose unverified hook targets."""
import hashlib
import json
from pathlib import Path

from bedrock_symbols.index import build_index


def test_126605_only_exposes_scoped_hook_candidates():
    root = Path(__file__).resolve().parents[1] / "data"
    directory = root / "1.26.60.5"
    candidate = json.loads((directory / "windows-x64.candidate.json").read_text(encoding="utf-8"))
    raw = (directory / "windows-x64.json").read_bytes()
    profile = json.loads(raw)
    entry = build_index(root)["versions"]["1.26.60.5"]["windows-x64"]
    assert profile["target"] == candidate["target"]
    assert profile["target"]["version"] == "1.26.60.5"
    assert profile["analysis"]["status"] == "partial_hooks_runtime_unverified"
    allowed = {"Keyboard::feed", "MouseDevice::feed", "LevelRendererPlayer::getFov",
               "ClientInstance::grabCursor", "ClientInstance::releaseCursor",
               "Actor::getNameTag", "MultiPlayerLevel::_subTick", "GameCore::keyboardEvent"}
    assert set(profile["symbols"]) == allowed
    for row in profile["symbols"].values():
        assert row["status"] == "candidate"
        assert row["abi_verified"] is False and row["runtime_verified"] is False
        assert len(row["entry_check"].split()) >= 24
    assert {key for key in profile if key.startswith("native_")} == {"native_keyboard"}
    keyboard = profile["native_keyboard"]
    assert keyboard["status"] == "candidate" and keyboard["runtime_verified"] is False
    assert (keyboard["owner_offset"], keyboard["processor_offset"],
            keyboard["message_offset"], keyboard["key_offset"]) == (0, 64, 16, 24)
    assert (keyboard["key_down_vtable_slot"], keyboard["key_up_vtable_slot"]) == (1, 2)
    assert profile["analysis"]["candidate_symbols"] == candidate["symbols"]
    assert all(row["status"] == "candidate" and row["abi_verified"] is False
               for row in candidate["symbols"].values())
    assert entry["symbols"] == len(allowed)
    assert entry["sha256"] == hashlib.sha256(raw).hexdigest()
    assert entry["module_sha256"] == profile["target"]["sha256"]
    assert json.loads((root / "manifest.json").read_text())["versions"]["1.26.60.5"]["windows-x64"] == entry
