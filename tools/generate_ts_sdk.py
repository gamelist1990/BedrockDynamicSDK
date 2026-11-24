#!/usr/bin/env python3
"""Package the hand-authored TypeScript SDK and generate vanilla catalog files."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
from typing import Any


CATEGORIES = {
    "block": "Block",
    "item": "Item",
    "entity": "Entity",
    "biome": "Biome",
    "effect": "Effect",
    "dimension": "Dimension",
}

CONSTANT_RE = re.compile(r'^\s+([A-Z][A-Za-z0-9_]*) = "(minecraft:[a-z0-9_./-]+)",?$')


def write_if_changed(path: Path, text: str) -> bool:
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return True


def load_vanilla_catalog(root: Path, game_version: str) -> tuple[str, str, dict[str, Any]]:
    manifest = json.loads((root / "data/manifest.json").read_text(encoding="utf-8"))
    version = game_version or manifest["default_version"]
    source_rel = manifest["versions"][version]["windows-x64"]["vanilla_data"]
    catalog = json.loads((root / source_rel).read_text(encoding="utf-8"))
    if catalog.get("schema") != 1 or set(catalog.get("ids", {})) != set(CATEGORIES):
        raise ValueError(f"Unexpected vanilla catalog schema in {source_rel}")
    return version, catalog["source"]["version"], catalog


def build_cpp_bootstrap_header(runtime_js: str) -> str:
    return (
        "// Embedded from sdk/ts/node_modules/@bedrock/sdk/index.js by tools/generate_ts_sdk.py.\n"
        "// Do not edit by hand.\n"
        "#pragma once\n\n"
        "namespace stageb {\n\n"
        'inline constexpr const char* kGeneratedBootstrap = R"STAGEB_BINDINGS(\n'
        + runtime_js
        + ')STAGEB_BINDINGS";\n\n'
        "} // namespace stageb\n"
    )


def build_vanilla_package_files(
    game_version: str, source_version: str, catalog: dict[str, Any]
) -> tuple[str, str, str]:
    header = f"// Generated from @minecraft/vanilla-data {source_version} for game {game_version}.\n// Do not edit by hand; re-run tools/generate_ts_sdk.py.\n"

    dts_lines = [
        header.rstrip(),
        f'export declare const dataVersion: "{source_version}";',
        "",
    ]
    js_lines = [
        header.rstrip(),
        '"use strict";',
        "Object.defineProperty(exports, '__esModule', { value: true });",
        f'exports.dataVersion = "{source_version}";',
        "",
    ]

    for kind, name in CATEGORIES.items():
        enum_name = f"Minecraft{name}Types"
        entries = sorted(catalog["ids"][kind].items())
        dts_lines.append(f"export declare const {enum_name}: {{")
        js_lines.append(f"const {enum_name} = Object.freeze({{")
        for constant, identifier in entries:
            if not CONSTANT_RE.fullmatch(f'    {constant} = "{identifier}"'):
                raise ValueError(f"Invalid {kind} entry: {constant!r} = {identifier!r}")
            dts_lines.append(f'  readonly {constant}: "{identifier}";')
            js_lines.append(f'  {constant}: "{identifier}",')
        dts_lines.append("};")
        dts_lines.append(f"export type {name}Id = (typeof {enum_name})[keyof typeof {enum_name}];")
        dts_lines.append(f"export declare function all{name}Ids(): ReadonlyArray<{name}Id>;\n")

        js_lines.append("});")
        js_lines.append(f"exports.{enum_name} = {enum_name};")
        js_lines.append(f"const _all{name}Ids = Object.freeze(Object.values({enum_name}));")
        js_lines.append(f"exports.all{name}Ids = function () {{ return _all{name}Ids; }};\n")

    pkg = {
        "name": "@bedrock/vanilla-data",
        "version": source_version,
        "description": f"Generated vanilla Bedrock identifier catalog ({source_version} for game {game_version}, with minecraft:item supplement)",
        "main": "./index.js",
        "types": "./index.d.ts",
        "exports": {
            ".": {
                "types": "./index.d.ts",
                "default": "./index.js",
            }
        },
        "sideEffects": False,
        "license": "MIT",
    }
    return "\n".join(dts_lines), "\n".join(js_lines), json.dumps(pkg, indent=2) + "\n"


def generate_all(
    root: Path,
    out_dir: Path,
    cpp_bootstrap_out: Path | None,
    game_version: str = "",
) -> dict[str, Path]:
    source = root / "sdk/ts/node_modules/@bedrock/sdk"
    sdk_dir = out_dir / "node_modules/@bedrock/sdk"
    runtime_js = (source / "index.js").read_text(encoding="utf-8")
    if sdk_dir.resolve() != source.resolve():
        for name in ("index.js", "index.d.ts", "package.json"):
            write_if_changed(sdk_dir / name, (source / name).read_text(encoding="utf-8"))

    resolved_game_ver, source_ver, catalog = load_vanilla_catalog(root, game_version)
    vanilla_dts, vanilla_js, vanilla_pkg = build_vanilla_package_files(
        resolved_game_ver, source_ver, catalog
    )
    vanilla_dir = out_dir / "node_modules" / "@bedrock" / "vanilla-data"
    write_if_changed(vanilla_dir / "index.d.ts", vanilla_dts)
    write_if_changed(vanilla_dir / "index.js", vanilla_js)
    write_if_changed(vanilla_dir / "package.json", vanilla_pkg)

    if cpp_bootstrap_out is not None:
        write_if_changed(cpp_bootstrap_out, build_cpp_bootstrap_header(runtime_js))

    return {
        "sdk_dir": sdk_dir,
        "vanilla_dir": vanilla_dir,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="Repository root directory",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=None,
        help="Output root containing node_modules/@bedrock/{sdk,vanilla-data} (defaults to <root>/sdk/ts)",
    )
    parser.add_argument(
        "--cpp-bootstrap",
        type=Path,
        default=None,
        help="Optional C++ header output for Stage B runtime bootstrap (defaults to <root>/exsample/ScriptClient/core/generated_sdk_bootstrap.hpp)",
    )
    parser.add_argument(
        "--no-cpp-bootstrap",
        action="store_true",
        help="Skip writing the C++ bootstrap header",
    )
    parser.add_argument(
        "--version",
        default="",
        help="Game version key in data/manifest.json (defaults to default_version)",
    )
    args = parser.parse_args()

    root = args.root.resolve()
    out_dir = args.out_dir.resolve() if args.out_dir else root / "sdk/ts"
    cpp_bootstrap: Path | None = None
    if not args.no_cpp_bootstrap:
        cpp_bootstrap = (
            args.cpp_bootstrap.resolve()
            if args.cpp_bootstrap
            else root / "exsample/ScriptClient/core/generated_sdk_bootstrap.hpp"
        )

    generate_all(root, out_dir, cpp_bootstrap, args.version)


if __name__ == "__main__":
    main()
