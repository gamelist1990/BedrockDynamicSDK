from __future__ import annotations

import hashlib
from pathlib import Path
from functools import cached_property

import pefile

IMAGE_FILE_MACHINE_AMD64 = 0x8664


def sha256_file(path: Path) -> str:
    with path.open("rb") as file:
        return hashlib.file_digest(file, "sha256").hexdigest()


class Executable:
    """PE image parsed from an unencrypted, valid on-disk executable."""

    def __init__(self, path: Path):
        self.path = Path(path)
        with self.path.open("rb") as handle:
            if handle.read(2) != b"MZ":
                raise ValueError(f"Not a usable PE image (missing MZ header): {self.path}. "
                                 "Some GDK/Proton installs contain encrypted executable files.")
        self.pe = pefile.PE(str(self.path), fast_load=True)
        if self.pe.FILE_HEADER.Machine != IMAGE_FILE_MACHINE_AMD64:
            raise ValueError(f"Expected Windows x64 (AMD64), got PE machine 0x{self.pe.FILE_HEADER.Machine:04x}")
        self.sha256 = sha256_file(self.path)
        self.image_base = int(self.pe.OPTIONAL_HEADER.ImageBase)
        self.image_size = int(self.pe.OPTIONAL_HEADER.SizeOfImage)
        self.text = self.find_section(".text")
        if self.text is None:
            raise ValueError("No .text section in PE image")
        self.text_rva = int(self.text.VirtualAddress)
        self.text_data = self.text.get_data()
        self.text_size = len(self.text_data)
        self.text_sha256 = hashlib.sha256(self.text_data).hexdigest()

    def find_section(self, name: str):
        for s in self.pe.sections:
            if s.Name.rstrip(b"\0").decode("latin1") == name:
                return s
        return None

    def text_bytes_at(self, rva: int, length: int) -> bytes:
        off = rva - self.text_rva
        if off < 0 or off >= len(self.text_data):
            return b""
        return self.text_data[off:off + length]

    def follow_export_thunk(self, rva: int) -> int:
        """Resolve up to four in-module E9 rel32 jump stubs to actual function code."""
        import struct
        seen = set()
        current = rva
        for _ in range(4):
            if current in seen:
                break
            seen.add(current)
            op = self.text_bytes_at(current, 5)
            if len(op) != 5 or op[0] != 0xE9:
                break
            dst = current + 5 + struct.unpack_from("<i", op, 1)[0]
            if not self.text_bytes_at(dst, 1):
                break
            current = dst
        return current

    def exports(self) -> list[tuple[str, int]]:
        self.pe.parse_data_directories(
            directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]]
        )
        directory = getattr(self.pe, "DIRECTORY_ENTRY_EXPORT", None)
        if directory is None:
            return []
        return [(s.name.decode("utf-8", "replace"), int(s.address))
                for s in directory.symbols if s.name and s.address]

    @cached_property
    def pdata_ranges(self) -> dict[int, int]:
        """Exact x64 RUNTIME_FUNCTION entry boundaries (RVA -> end RVA)."""
        pdata = self.find_section(".pdata")
        if pdata is None:
            return {}
        import struct
        raw = pdata.get_data()
        return {start: end for start, end, unwind
                in struct.iter_unpack("<III", raw[:(len(raw) // 12) * 12])
                if self.text_rva <= start < self.text_rva + self.text_size
                and start < end <= self.text_rva + self.text_size}

    def pdata_bounds(self) -> set[int]:
        return set(self.pdata_ranges)

    def function_end(self, rva: int) -> int | None:
        return self.pdata_ranges.get(rva)
