"""Build portable ZIP fixtures and run the ScriptClient host install tests."""
import json
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo


def fixture(path: Path, extra=(), marker=True, game_version="*", mod_version="1.0.0",
            name="ManagerTest", label="test"):
    with ZipFile(path, "w", ZIP_DEFLATED) as archive:
        if marker:
            archive.writestr("scriptclient-package.json", '{"format":"scriptclient-mod","version":1}')
        archive.writestr("mod/manifest.json", json.dumps({
            "name": name, "entry": "index.ts", "version": mod_version,
            "apiVersion": "1.0.0", "gameVersions": [game_version],
        }))
        archive.writestr("mod/index.ts", '''
const text: string = "{}";
export function onEnable() { console.log(text + "-enabled"); }
export function onDisable() { console.log(text + "-disabled"); }
'''.replace("{}", label))
        for name, data in extra:
            archive.writestr(name, data)


with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    link = ZipInfo("mod/link")
    link.create_system = 3
    link.external_attr = (stat.S_IFLNK | 0o777) << 16
    fixture(root / "valid.mods", [("mod/obsolete.txt", "remove on update")])
    fixture(root / "v2.mods", name="managertest", mod_version="2.0.0", label="v2")
    fixture(root / "v3.mods", mod_version="3.0.0", label="v3")
    fixture(root / "traversal.zip", [("mod/../sentinel.ts", "")])
    fixture(root / "case.mods", [("mod/Index.ts", "")])
    fixture(root / "no-marker.zip", marker=False)
    fixture(root / "symlink.mods", [(link, "index.ts")])
    fixture(root / "native.mods", [("mod/native.DLL", "")])
    fixture(root / "device.mods", [("mod/com1.txt", "")])
    fixture(root / "incompatible.mods", game_version="0.0.0")
    subprocess.run([sys.argv[1], "--manager", tmp], check=True)
