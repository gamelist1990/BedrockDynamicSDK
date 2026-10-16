"""Read compiler-embedded MSVC RTTI TypeDescriptor names from Windows x64 PE images.

RTTI is **incomplete** by design: only types with emitted TypeDescriptors appear.
Names are compiler-decorated RTTI strings, not full verified class layouts.
"""
from __future__ import annotations
import re
import struct
from .pe import Executable

# .?AV...@@ / .?AU...@@, ASCII decorated strings.
_RTTI_NAME = re.compile(rb"\.\?A[UV][A-Za-z0-9_?$@<>\-]{3,220}@@\x00")


def extract_rtti(exe: Executable) -> dict:
    classes: list[dict] = []
    for section in exe.pe.sections:
        # Most GDK MSVC TypeDescriptors appear in .data rather than .rdata.
        if section.Name.rstrip(b"\x00") not in (b".data", b".rdata"):
            continue
        content = section.get_data()
        for match in _RTTI_NAME.finditer(content):
            if match.start() < 16:
                continue
            pos = match.start()
            vftable, spare = struct.unpack_from("<QQ", content, pos - 16)
            # A valid type descriptor references a common compiler type_info vftable.
            if spare != 0:
                continue
            if not (exe.image_base < vftable < exe.image_base + exe.image_size):
                continue
            text = match.group()[:-1].decode("ascii")
            name = text[4:-2]  # Drop '.?AV' or '.?AU', plus terminal '@@'.
            classes.append({
                "decorated": text,
                "name_hint": name,
                "kind": "class" if text[3] == "V" else "struct",
                "rtti_type_descriptor_rva": f"0x{section.VirtualAddress + pos - 16:X}",
                "source_section": section.Name.rstrip(b"\x00").decode("ascii"),
            })
    classes.sort(key=lambda entry: entry["rtti_type_descriptor_rva"])
    return {
        "schema": 1,
        "target": {
            "version": "version-provided-by-cli",
            "sha256": exe.sha256,
            "text_sha256": exe.text_sha256,
            "platform": "windows",
            "arch": "x64",
        },
        "note": "TypeDescriptor names extracted from MSVC RTTI; not field offsets or complete SDK classes.",
        "count": len(classes),
        "types": classes,
    }
