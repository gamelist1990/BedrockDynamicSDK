"""Keep approved entry verification separate from scoped ABI/runtime confirmation."""
import hashlib
import json
from pathlib import Path

from bedrock_symbols.index import build_index


def test_126605_entry_verified_symbols_keep_runtime_scope_to_keyboard():
    root = Path(__file__).resolve().parents[1] / "data"
    directory = root / "1.26.60.5"
    raw = (directory / "windows-x64.json").read_bytes()
    profile = json.loads(raw)
    assert not (directory / "windows-x64.candidate.json").exists()
    assert profile["target"]["version"] == "1.26.60.5"
    assert profile["analysis"]["status"] == "entry_verified_runtime_partial"
    assert profile["analysis"]["runtime_verified"] is False
    assert profile["analysis"]["runtime_verified_symbols"] == ["Keyboard::feed"]
    assert len(profile["symbols"]) == 43
    verified = {name for name, row in profile["symbols"].items() if row["status"] == "verified"}
    assert verified == set(profile["symbols"])
    for name, row in profile["symbols"].items():
        assert row["abi_verified"] is (name == "Keyboard::feed")
        assert row["runtime_verified"] is (name == "Keyboard::feed")
        assert row["verification_scope"] == ("runtime-hook" if name == "Keyboard::feed" else "entry-guard")
        assert row["entry_validation"]["status"] == "complete"
        assert row["entry_validation"]["entry_guard_hit"] is True
        assert row["entry_validation"]["readable"] is True
    keyboard = profile["symbols"]["Keyboard::feed"]
    evidence = keyboard["runtime_validation"]
    assert keyboard["rva"] == "0x331C20"
    assert keyboard["abi"] == "bool __fastcall(int inputKey, bool down)"
    assert evidence["status"] == "complete"
    assert evidence["exe_sha256"] == profile["target"]["sha256"]
    assert evidence["text_sha256"] == profile["target"]["text_sha256"]
    assert evidence["static_evidence_ids"]
    assert evidence["observation_samples"] == 10
    assert evidence["minimum_observed_calls"] == 97
    assert evidence["observed_down_values"] == [0, 1]
    assert evidence["observed_return_values"] == [0]
    assert len(evidence["source_sha256"]["events.jsonl"]) == 64
    assert evidence["limitations"]
    assert profile["analysis"]["runtime_validation"]["hook_execution"]["runtime_unverified_symbol_count"] == 42

    # Observing Keyboard::feed does not establish the GameCore context/object ABI.
    native_keyboard = profile["layouts"]["GameCore"]
    assert native_keyboard["status"] == "candidate"
    assert native_keyboard["runtime_verified"] is False
    assert (native_keyboard["owner_offset"], native_keyboard["processor_offset"],
            native_keyboard["message_offset"], native_keyboard["key_offset"]) == (0, 64, 16, 24)
    assert (native_keyboard["key_down_vtable_slot"], native_keyboard["key_up_vtable_slot"]) == (1, 2)
    entry = build_index(root)["versions"]["1.26.60.5"]["windows-x64"]
    assert entry["symbols"] == 43
    assert entry["sha256"] == hashlib.sha256(raw).hexdigest()
    assert entry["module_sha256"] == profile["target"]["sha256"]
    assert json.loads((root / "manifest.json").read_text())["versions"]["1.26.60.5"]["windows-x64"] == entry
