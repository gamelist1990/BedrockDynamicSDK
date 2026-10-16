"""Historical SDK layout hints from Latite source, NOT offsets validated for current PE."""
from __future__ import annotations
import re
from pathlib import Path

FIELD = re.compile(r"CLASS_FIELD\s*\(\s*(.+?)\s*,\s*(\w+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*\)")
VIRTUAL = re.compile(r"SDK::ClientInstance::(\w+)\s*\([^)]*\)\s*\{([\s\S]*?)\n\}",re.M)
CALL_VIRTUAL = re.compile(r"memory::callVirtual<[\s\S]*?>\s*\(this,\s*(0x[0-9A-Fa-f]+|\d+)")
FILES = {
    "ClientInstance": "src/mc/common/client/game/ClientInstance.h",
    "GuiData": "src/mc/common/client/gui/GuiData.h",
    "ClientInputHandler": "src/mc/common/client/input/ClientInputHandler.h",
    "MinecraftGame": "src/mc/common/client/game/MinecraftGame.h",
}

def extract_layout_hints(repository: Path, commit: str) -> dict:
    classes = {}
    for class_name, filename in FILES.items():
        text = (repository / filename).read_text(encoding="utf-8-sig")
        # Only the requested class block, never fields from adjacent helper types.
        block = re.search(r"\bclass\s+"+re.escape(class_name)+r"\s*\{([\s\S]*?)\n\s*\};", text)
        scoped = block.group(1) if block else ""
        members = [
            {"name":name,"cpp_type_hint":cpp_type.strip(),
             "offset":f"0x{int(offset,0):X}", "status":"historical_unverified"}
            for cpp_type,name,offset in FIELD.findall(scoped)
        ]
        classes[class_name] = {"members":members,"source":filename}
    cpp = (repository/"src/mc/common/client/game/ClientInstance.cpp").read_text(encoding="utf-8-sig")
    methods = []
    for method,body in VIRTUAL.findall(cpp):
        slot = CALL_VIRTUAL.search(body)
        if slot:
            methods.append({"name":method,"vtable_slot":f"0x{int(slot.group(1),0):X}",
                            "status":"historical_unverified"})
    classes["ClientInstance"]["virtual_methods"] = methods
    return {
        "schema":1,
        "source":f"LatiteClient/Latite@{commit}",
        "status":"historical_unverified",
        "note":"Offsets and vtable indices are from Latite's historical SDK and must NOT be used in 1.26.52.3 until independently validated.",
        "classes":classes,
    }
