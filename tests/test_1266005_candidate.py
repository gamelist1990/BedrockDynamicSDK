"""An incomplete native profile must not become a live hook catalog entry."""
import json
from pathlib import Path

from bedrock_symbols.index import build_index


def test_1266005_stays_off_live_catalog():
    root = Path(__file__).resolve().parents[1] / "data"
    directory = root / "1.26.6005.0"
    candidate = json.loads((directory / "windows-x64.candidate.json").read_text(encoding="utf-8"))
    assert candidate["target"]["version"] == "1.26.6005.0"
    assert candidate["target"]["file_version"] == "1.26.60.5"
    assert candidate["analysis"]["status"] == "offline_candidate_only"
    assert not (directory / "windows-x64.json").exists()
    assert "1.26.6005.0" not in build_index(root)["versions"]
    assert candidate["symbols"]
    assert all(row["status"] == "candidate" and row["abi_verified"] is False
               for row in candidate["symbols"].values())
    assert not any(key.startswith("native_") for key in candidate)
