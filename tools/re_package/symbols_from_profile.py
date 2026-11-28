"""Export re_package reference symbols from a canonical SDK profile."""

import argparse
import hashlib
import json
from pathlib import Path

from tools.bedrock_symbols.profile_parts import load_profile


def number(value):
    return int(value, 0) if isinstance(value, str) else int(value)


def reference_symbols(profile):
    symbols = []
    for name, definition in profile["symbols"].items():
        rva = number(definition["rva"])
        if rva <= 0:
            raise ValueError(f"Invalid RVA for {name}: {rva}")
        symbols.append({"name": name, "rva": rva})
    if not symbols:
        raise ValueError("The reference profile defines no symbols")
    return sorted(symbols, key=lambda symbol: (symbol["rva"], symbol["name"]))


def verify_reference(path, target):
    import pefile

    # Transfer builds can differ outside .text from the profiled executable.
    # Match the code hash and RVA geometry before applying reference symbols.
    pe = pefile.PE(str(path), fast_load=True)
    try:
        text = next((s for s in pe.sections if s.Name.rstrip(b"\0") == b".text"), None)
        if (
            pe.FILE_HEADER.Machine != 0x8664
            or pe.OPTIONAL_HEADER.ImageBase != number(target["image_base"])
            or pe.OPTIONAL_HEADER.SizeOfImage != number(target["size_of_image"])
            or text is None
            or text.VirtualAddress != number(target["text_rva"])
            or text.SizeOfRawData != number(target["text_size"])
            or hashlib.sha256(text.get_data()).hexdigest() != target["text_sha256"]
        ):
            raise ValueError(f"{path} does not match the reference profile's code and PE layout")
    finally:
        pe.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path, help="Windows x64 profile manifest.json")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reference-exe", type=Path, help="Verify code identity and PE layout")
    args = parser.parse_args()
    try:
        profile = load_profile(args.manifest)
        symbols = reference_symbols(profile)
        if args.reference_exe:
            verify_reference(args.reference_exe, profile["target"])
        args.output.write_text(json.dumps(symbols, indent=2) + "\n", encoding="utf-8")
    except (KeyError, OSError, ValueError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    main()
