#!/usr/bin/env python3
"""Import pinned Mojang vanilla-data IDs; generate C++ and TypeScript from the same catalog."""

import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import tarfile


CATEGORIES = {
    "block": "Block", "item": "Item", "entity": "Entity",
    "biome": "Biome", "effect": "Effect", "dimension": "Dimension",
}
CONSTANT = re.compile(r'^\s+([A-Z][A-Za-z0-9_]*) = "(minecraft:[a-z0-9_./-]+)",?$')

# Bedrock entity identifiers missing from Mojang's published declaration.
# `item` is the dropped stack entity, not an inventory item identifier.
ENTITY_SUPPLEMENTS = {"Item": "minecraft:item"}


def supplement_entities(entries):
    for constant, identifier in ENTITY_SUPPLEMENTS.items():
        if identifier in entries.values():
            continue
        if constant in entries:
            raise ValueError(f"entity supplement conflicts with upstream {constant!r}")
        entries[constant] = identifier


def write(path, text):
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def import_package(archive, output):
    integrity = base64.b64encode(hashlib.sha512(archive.read_bytes()).digest()).decode()
    with tarfile.open(archive) as package:
        metadata = json.load(package.extractfile("package/package.json"))
        if metadata["name"] != "@minecraft/vanilla-data" or metadata["license"] != "MIT":
            raise ValueError("expected Mojang's MIT vanilla-data package")
        catalog = {}
        for kind, name in CATEGORIES.items():
            source = package.extractfile(f"package/lib/mojang-{kind}.d.ts").read().decode()
            declaration = source.split(f"enum Minecraft{name}Types {{\n", 1)[1].split("\n}", 1)[0]
            lines = declaration.splitlines()
            entries = {}
            for line in lines:
                match = CONSTANT.fullmatch(line)
                if not match or match[1] in entries:
                    raise ValueError(f"unrecognized or duplicate {kind} entry: {line!r}")
                entries[match[1]] = match[2]
            if not entries:
                raise ValueError(f"empty {kind} catalog")
            catalog[kind] = entries

    supplement_entities(catalog["entity"])
    data = {"schema": 1, "source": {"package": metadata["name"], "version": metadata["version"],
                                   "integrity": "sha512-" + integrity, "license": "MIT"},
            "ids": catalog}
    write(output, json.dumps(data, indent=2) + "\n")


def generate(root, version, cpp, typescript):
    manifest = json.loads((root / "data/manifest.json").read_text(encoding="utf-8"))
    version = version or manifest["default_version"]
    source = manifest["versions"][version]["windows-x64"]["vanilla_data"]
    catalog = json.loads((root / source).read_text(encoding="utf-8"))
    if catalog["schema"] != 1 or set(catalog["ids"]) != set(CATEGORIES):
        raise ValueError(f"unexpected catalog schema in {source}")
    source_version = catalog["source"]["version"]
    header = f"// Generated from @minecraft/vanilla-data {source_version} for game {version}.\n"
    cpp_lines = [header, "#pragma once\n#include <string_view>\n", "namespace bedrock::sdk::vanilla {",
                 "template<class Category>",
                 "class TypeId {",
                 "public:",
                 "    explicit constexpr TypeId(std::string_view id) : id_(id) {}",
                 "    [[nodiscard]] constexpr std::string_view value() const noexcept { return id_; }",
                 "    constexpr bool operator==(const TypeId&) const = default;",
                 "private:",
                 "    std::string_view id_;",
                 "};\n",
                 f'inline constexpr std::string_view dataVersion = "{source_version}";']
    ts_lines = [header, f'export const dataVersion = "{source_version}";']
    for kind, name in CATEGORIES.items():
        enum = f"Minecraft{name}Types"
        cpp_lines.extend([f"struct {name}Category;", f"using {name}Id = TypeId<{name}Category>;",
                          f"namespace {enum} {{"])
        ts_lines.append(f"export const {enum} = {{")
        for constant, identifier in sorted(catalog["ids"][kind].items()):
            if not CONSTANT.fullmatch(f'    {constant} = "{identifier}"'):
                raise ValueError(f"invalid {kind} entry: {constant!r} = {identifier!r}")
            cpp_lines.append(f'inline constexpr {name}Id {constant}{{"{identifier}"}};')
            ts_lines.append(f'  {constant}: "{identifier}",')
        cpp_lines.append("}\n")
        ts_lines.extend(["} as const;", f"export type {name}Id = typeof {enum}[keyof typeof {enum}];\n"])
    cpp_lines.append("} // namespace bedrock::sdk::vanilla\n")
    write(cpp, "\n".join(cpp_lines))
    write(typescript, "\n".join(ts_lines))


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    commands = cli.add_subparsers(dest="command", required=True)
    importer = commands.add_parser("import")
    importer.add_argument("archive", type=Path)
    importer.add_argument("output", type=Path)
    generator = commands.add_parser("generate")
    generator.add_argument("--root", type=Path, required=True)
    generator.add_argument("--version", default="")
    generator.add_argument("--cpp", type=Path, required=True)
    generator.add_argument("--typescript", type=Path, required=True)
    args = cli.parse_args()
    if args.command == "import":
        import_package(args.archive, args.output)
    else:
        generate(args.root, args.version, args.cpp, args.typescript)


if __name__ == "__main__":
    main()
