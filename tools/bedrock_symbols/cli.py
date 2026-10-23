from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .generator import make_database, port_database, write_json
from .pe import Executable
from .index import build_index
from .lens import targets_from_lens
from .rtti import extract_rtti
from .client_profile import make_client_profile
from .layout_hints import extract_layout_hints
from .embed import make_header
from .bundle import build_unified_profile


def main() -> int:
    p = argparse.ArgumentParser(prog="bedrock-symbols", description="Offline Bedrock PE symbol generation")
    sub = p.add_subparsers(dest="command", required=True)
    inspect = sub.add_parser("inspect", help="Inspect PE identity and metadata")
    inspect.add_argument("binary", type=Path)
    generate = sub.add_parser("generate", help="Generate database using PE exports and verified target RVAs")
    generate.add_argument("binary", type=Path)
    generate.add_argument("--version", required=True)
    generate.add_argument("--kind", choices=["client", "server"], default="client")
    generate.add_argument("--targets", type=Path)
    generate.add_argument("--no-exports", action="store_true")
    generate.add_argument("--max-entries", type=int, default=50)
    generate.add_argument("--output", type=Path, required=True)
    port = sub.add_parser("port", help="Port candidates with source-unique anchors and destination .pdata boundaries")
    port.add_argument("binary", type=Path)
    port.add_argument("--old", type=Path, required=True)
    port.add_argument("--version", required=True)
    port.add_argument("--output", type=Path, required=True)
    embed = sub.add_parser("embed", help="Embed a symbol JSON database into a single-DLL C++ header")
    embed.add_argument("database", type=Path)
    embed.add_argument("--output", type=Path, required=True)
    hints = sub.add_parser("layout-hints", help="Collect explicitly UNVERIFIED SDK offsets from Latite")
    hints.add_argument("--latite-repo", type=Path, required=True)
    hints.add_argument("--latite-commit", default="9f7463515dd298a496da918285936d78c7416aad")
    hints.add_argument("--output", type=Path, required=True)
    client = sub.add_parser("client-profile", help="Generate Latite-oriented runtime symbol data from current PE")
    client.add_argument("binary", type=Path)
    client.add_argument("--latite-addresses", type=Path, required=True)
    client.add_argument("--version", required=True)
    client.add_argument("--latite-commit", default="9f74635")
    client.add_argument("--output", type=Path, required=True)
    client.add_argument("--compatibility-output", type=Path)
    client.add_argument("--latite-repo", type=Path,
                        help="Also include RTTI and SDK hints directly in windows-x64.json")
    rtti = sub.add_parser("rtti", help="Generate MSVC RTTI type name data from an x64 PE")
    rtti.add_argument("binary", type=Path)
    rtti.add_argument("--version", required=True)
    rtti.add_argument("--output", type=Path, required=True)
    lens = sub.add_parser("lens-import", help="Generate signatures for function names/RVAs from Bedrock Lens SQLite")
    lens.add_argument("binary", type=Path)
    lens.add_argument("--database", type=Path, required=True)
    lens.add_argument("--artifact-id", type=int, required=True)
    lens.add_argument("--version", required=True)
    lens.add_argument("--kind", choices=["client", "server"], default="client")
    lens.add_argument("--name-contains")
    lens.add_argument("--include-anonymous", action="store_true")
    lens.add_argument("--limit", type=int, default=100)
    lens.add_argument("--output", type=Path, required=True)
    index = sub.add_parser("index", help="Generate GitHub-friendly version directory index")
    index.add_argument("data_directory", type=Path)
    index.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    try:
        if args.command == "embed":
            make_header(args.database,args.output)
            print(json.dumps({"embedded":str(args.output)}))
            return 0
        if args.command == "layout-hints":
            output = extract_layout_hints(args.latite_repo,args.latite_commit)
            write_json(args.output,output)
            print(json.dumps({"output":str(args.output),"classes":len(output["classes"])},indent=2))
            return 0
        if args.command == "index":
            output = build_index(args.data_directory)
            write_json(args.output, output)
            print(json.dumps({"output": str(args.output),
                              "versions": len(output["versions"])}, indent=2))
            return 0
        pe = Executable(args.binary)
        if args.command == "inspect":
            output = {
                "source": str(pe.path),
                "sha256": pe.sha256, "text_sha256": pe.text_sha256,
                "image_base": hex(pe.image_base), "image_size": pe.image_size,
                "text_rva": hex(pe.text_rva), "text_size": pe.text_size,
                "export_count": len(pe.exports()), "pdata_function_count": len(pe.pdata_bounds())
            }
        elif args.command == "client-profile":
            if args.latite_repo:
                output = build_unified_profile(
                    pe, args.version, args.latite_addresses,
                    args.latite_repo, args.latite_commit)
                compatibility = output["analysis"]["latite_compatibility"]
            else:
                output, compatibility = make_client_profile(
                    pe, args.version, args.latite_addresses, args.latite_commit)
            write_json(args.output, output)
            if args.compatibility_output:
                write_json(args.compatibility_output, compatibility)
            print(json.dumps({
                "output": str(args.output), "symbols": len(output["symbols"]),
                "latite_matched": compatibility["matched"],
                "rtti_types": output.get("analysis", {}).get("rtti", {}).get("count", 0)
            }, indent=2))
            return 0
        elif args.command == "rtti":
            output = extract_rtti(pe)
            output["target"]["version"] = args.version
            write_json(args.output, output)
            print(json.dumps({"output": str(args.output), "types": output["count"]}, indent=2))
            return 0
        elif args.command == "lens-import":
            targets = targets_from_lens(
                args.database, args.artifact_id, pe.sha256, args.limit,
                args.name_contains, args.include_anonymous
            )
            output = make_database(pe, args.version, args.kind, targets,
                                   include_exports=False, max_entries=len(targets))
            write_json(args.output, output)
        elif args.command == "generate":
            targets = json.loads(args.targets.read_text())["targets"] if args.targets else []
            output = make_database(pe, args.version, args.kind, targets=targets,
                                   include_exports=not args.no_exports, max_entries=args.max_entries)
            write_json(args.output, output)
        else:
            old = json.loads(args.old.read_text(encoding="utf-8"))
            output = port_database(old, pe, args.version)
            write_json(args.output, output)
        print(json.dumps(output if args.command == "inspect" else {
            "output": str(args.output), "symbols": len(output["symbols"]),
            "warnings": output["generation"]["warnings"][:10]
        }, ensure_ascii=False, indent=2))
        return 0
    except (ValueError, KeyError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
