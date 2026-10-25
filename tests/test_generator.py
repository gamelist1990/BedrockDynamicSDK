from __future__ import annotations

import json
from pathlib import Path

import pytest
from bedrock_symbols.signature import (
    generate_unique_signature, parse_pattern, pattern_string, unique_match
)
from bedrock_symbols.pe import Executable
from bedrock_symbols.generator import make_database, port_database


def test_masked_signature_unique_and_ambiguous():
    code = bytes.fromhex("55 48 89 E5 48 83 EC 28 48 8B 05 12 34 56 78 E8 10 00 00 00 90 C3")
    payload = b"\xCC" * 40 + code + b"\xCC" * 40
    result = generate_unique_signature(code, payload, 40)
    assert result
    signature, consumed = result
    assert consumed >= 16
    assert "??" in signature
    assert unique_match(payload, signature) == 40
    assert unique_match(payload + payload, signature) is None
    assert pattern_string(*parse_pattern(signature)) == signature


def test_unique_signature_masks_rsp_relative_stack_offsets():
    # Frame size and RSP-relative argument positions move between builds.
    code = bytes.fromhex(
        "48 83 EC 48 0F B7 84 24 C0 00 00 00 "
        "0F B7 8C 24 B8 00 00 00 C3")
    payload = b"\xCC" * 40 + code + b"\xCC" * 40
    result = generate_unique_signature(code, payload, 40)
    assert result
    signature, _ = result
    assert signature.startswith("48 83 EC ??")
    assert "0F B7 84 24 ?? ?? ?? ??" in signature
    assert "0F B7 8C 24 ?? ?? ?? ??" in signature


def test_pattern_validation():
    for invalid in ("", "12 GG", "12 123", "? AB B"):
        with pytest.raises(ValueError):
            parse_pattern(invalid)
    assert unique_match(b"\x12\x34\x56\x78", "12 34 56 78") == 0
    assert unique_match(b"\x12\x34\x56\x78", "?? ??") is None


class FakePE:
    def __init__(self):
        self.text_rva = 0x1000
        self.text_data = b"\xCC"*20 + bytes.fromhex(
            "55 48 89 E5 48 83 EC 28 48 8B 05 12 34 56 78 E8 10 00 00 00 90 C3"
        ) + b"\xCC"*40
        self.text_size = len(self.text_data)
        self.text_sha256 = "f" * 64
        self.sha256 = "a" * 64
        self.image_base = 0x140000000
        self.image_size = 0x4000
        self.path = Path("synthetic.exe")

    def text_bytes_at(self, rva, length):
        i = rva - self.text_rva
        return self.text_data[i:i+length] if 0 <= i < len(self.text_data) else b""

    def exports(self):
        return [("Fake::test", self.text_rva + 20)]

    def follow_export_thunk(self, rva):
        return rva

    def pdata_bounds(self):
        return {self.text_rva + 20}

    def function_end(self, rva):
        return None


def test_generator_and_version_port():
    binary = FakePE()
    old = make_database(binary, "1.26.52.3", targets=[
        {"name": "manual", "rva": "0x1014", "verified": False,
         "source_text_sha256": "f"*64},
        {"name": "wrong-build", "rva": "0x1014",
         "source_text_sha256": "0"*64}
    ])
    assert "Fake::test" in old["symbols"]
    assert old["symbols"]["Fake::test"]["status"] == "verified"
    assert old["symbols"]["manual"]["status"] == "candidate"
    assert "wrong-build" not in old["symbols"]
    assert any("does not match" in w for w in old["generation"]["warnings"])
    ported = port_database(old, binary, "1.26.99.1")
    assert ported["symbols"]["Fake::test"]["status"] == "candidate"
    assert ported["target"]["version"] == "1.26.99.1"


def test_encrypted_game_file_rejected(tmp_path):
    exe = tmp_path / "Minecraft.Windows.exe"
    exe.write_bytes(b"encrypted-via-gdk-xvc")
    with pytest.raises(ValueError, match="missing MZ"):
        Executable(exe)


def test_manifest_index(tmp_path):
    from bedrock_symbols.index import build_index
    data = tmp_path/"data"/"1.26.52.3"
    data.mkdir(parents=True)
    (data/"windows-x64.json").write_text(json.dumps({
      "schema": 1,
      "target": {"version": "1.26.52.3", "arch": "x64", "sha256": "a"*64,
                 "text_sha256": "b"*64,"size_of_image":2048,
                 "text_rva":"0x100", "text_size":256},
      "symbols": {"alpha": {"rva": "0x10"}}
    }))
    idx = build_index(tmp_path/"data")
    assert idx["versions"]["1.26.52.3"]["windows-x64"]["symbols"] == 1
    assert idx["versions"]["1.26.52.3"]["windows-x64"]["path"] == "data/1.26.52.3/windows-x64.json"
    (data/"windows-x64.json").write_text('{"target": {"version": "broken", "arch": "x64"}}')
    with pytest.raises(ValueError):
        build_index(tmp_path/"data")


def test_lens_catalog_bridge_requires_matching_sha(tmp_path):
    import sqlite3
    from bedrock_symbols.lens import targets_from_lens
    catalog = tmp_path / "lens.db"
    db = sqlite3.connect(catalog)
    db.executescript("""
       CREATE TABLE artifacts (id INTEGER, sha256 TEXT);
       CREATE TABLE functions (
         artifact_id INTEGER, rva INTEGER, name TEXT, namespace TEXT
       );
    """)
    db.execute("INSERT INTO artifacts VALUES (?, ?)", (2, "a"*64))
    db.executemany("INSERT INTO functions VALUES (?, ?, ?, ?)", [
        (2, 0x1200, "ClientInstance::getLocalPlayer", "ClientInstance"),
        (2, 0x2000, "FUN_2000", ""),
        (2, 0x3000, "Actor::getNameTag", "Actor"),
    ])
    db.commit()
    db.close()
    with pytest.raises(ValueError, match="SHA256"):
        targets_from_lens(catalog, 2, "b"*64)
    rows = targets_from_lens(catalog, 2, "a"*64)
    assert len(rows) == 2
    assert rows[0]["verified"] is False
    assert [r["name"] for r in targets_from_lens(
      catalog, 2, "a"*64, name_contains="Actor")] == ["Actor::getNameTag"]


def test_embed_and_client_profile_metadata(tmp_path):
    from bedrock_symbols.embed import make_header
    data = tmp_path / "profile.json"
    data.write_text(json.dumps({"schema": 1, "symbols": {"ClientInstance::grabCursor": {}}}))
    out = tmp_path / "generated.hpp"
    make_header(data, out)
    text = out.read_text()
    assert "inline constexpr const char* kSymbolDatabase" in text
    assert "ClientInstance::grabCursor" in text
    invalid = tmp_path / "invalid.json"
    invalid.write_text('{"schema": 17, "symbols": {}}')
    with pytest.raises(ValueError):
        make_header(invalid, out)


def test_historical_latite_parser_and_category(tmp_path):
    from bedrock_symbols.latite import parse_latite_addresses
    from bedrock_symbols.client_profile import feature_for
    source = tmp_path / "Addresses.h"
    source.write_text('''
    inline static SigImpl MainWindow__windowProcCallback {
      [](memory::signature_store&, uintptr_t res) { return res; },
      "55 48 89 E5 ?? E8 ? ? ? ?"_sig,
      "MainWindow::_windowProcCallback"
    };
    ''')
    rows = parse_latite_addresses(source)
    assert len(rows) == 1
    assert rows[0]["key"] == "MainWindow__windowProcCallback"
    assert rows[0]["address_mode"] == "direct"
    assert rows[0]["description"] == "MainWindow::_windowProcCallback"
    assert feature_for(rows[0]["key"]) in {"core", "input"}


def test_real_profile_marks_only_disassembled_abi_verified():
    here = Path(__file__).resolve().parents[1]
    path = here / "data/1.26.52.3/windows-x64.json"
    if not path.exists():
        pytest.skip("Generated project profile not checked in")
    data = json.loads(path.read_text())
    # Keep this exact-build profile focused on symbols currently consumed by
    # the SDK and public examples; historical/unused signatures live in the
    # analysis archive, not the active symbol table.
    expected = {
        "LevelRendererPlayer::getFov",
        "ClientInstance::grabCursor",
        "ClientInstance::releaseCursor",
        "MultiPlayerLevel::_subTick",
        "MouseDevice::feed",
        "GameCore::keyboardEvent",
        "Keyboard::feed",
        "GameCore::handleMouseInput",
        "Actor::getNameTag",
        "ItemStackBase::getHoverName",
        "Level::getRuntimeActorList",
        "ClientInstance::onPlayerLoaded",
        "ClientInstance::onLevelExit",
        "ClientInstance::onDimensionChangedEvent",
        "ClientInstance::onCancelJoinGame",
    }
    assert set(data["symbols"]) == expected
    verified = {
        "MouseDevice::feed", "Keyboard::feed", "GameCore::keyboardEvent",
        "Actor::getNameTag", "LevelRendererPlayer::getFov",
        "Level::getRuntimeActorList", "ItemStackBase::getHoverName",
        "ClientInstance::grabCursor", "ClientInstance::releaseCursor",
        "MultiPlayerLevel::_subTick", "GameCore::handleMouseInput",
    }
    for name, symbol in data["symbols"].items():
        if name in verified:
            assert symbol["status"] == "verified"
            assert symbol["abi_verified"] is True
        else:
            assert symbol["status"] == "candidate", name
            assert symbol["abi_verified"] is False
        assert symbol["signature"]
