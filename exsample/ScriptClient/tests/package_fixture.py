"""Build portable ZIP fixtures and run the ScriptClient host install tests."""
import json
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo


def fixture(path: Path, extra=(), marker=True, version="*"):
    with ZipFile(path, "w", ZIP_DEFLATED) as archive:
        if marker:
            archive.writestr("scriptclient-package.json", '{"format":"scriptclient-mod","version":1}')
        archive.writestr("mod/manifest.json", json.dumps({
            "name": "ManagerTest", "entry": "index.ts", "version": "1.0.0",
            "apiVersion": "1.0.0", "gameVersions": [version],
        }))
        archive.writestr("mod/index.ts", '''
const text: string = "test";
export function onEnable() { console.log(text + "-enabled"); }
export function onDisable() { console.log(text + "-disabled"); }
''')
        for name, data in extra:
            archive.writestr(name, data)


with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    link = ZipInfo("mod/link")
    link.create_system = 3
    link.external_attr = (stat.S_IFLNK | 0o777) << 16
    fixture(root / "valid.mods")
    fixture(root / "traversal.zip", [("mod/../sentinel.ts", "")])
    fixture(root / "case.mods", [("mod/Index.ts", "")])
    fixture(root / "no-marker.zip", marker=False)
    fixture(root / "symlink.mods", [(link, "index.ts")])
    fixture(root / "native.mods", [("mod/native.DLL", "")])
    fixture(root / "device.mods", [("mod/com1.txt", "")])
    fixture(root / "incompatible.mods", version="0.0.0")
    subprocess.run([sys.argv[1], "--manager", tmp], check=True)
