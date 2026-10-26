"""Check x64 PE exception tables before malformed records can hide C++ handlers."""

from pathlib import Path
import struct
import sys


def check(data):
    """Return the runtime-function count; raise ValueError for a malformed image."""
    def unpack(fmt, offset):
        try:
            return struct.unpack_from("<" + fmt, data, offset)
        except struct.error as error:
            raise ValueError("truncated PE image") from error

    if data[:2] != b"MZ":
        raise ValueError("missing DOS signature")
    pe, = unpack("I", 0x3c)
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("missing PE signature")
    machine, section_count = unpack("HH", pe + 4)
    if machine != 0x8664:
        raise ValueError("expected an x86_64 PE image")
    optional_size, = unpack("H", pe + 20)
    optional = pe + 24
    magic, = unpack("H", optional)
    if magic != 0x20b or optional_size < 144:
        raise ValueError("expected PE32+ exception directory")
    directory_count, = unpack("I", optional + 108)
    if directory_count < 4:
        raise ValueError("missing exception directory")
    table, size = unpack("II", optional + 112 + 3 * 8)
    if not table or not size or size % 12:
        raise ValueError("missing or truncated runtime-function table")
    sections = []
    for index in range(section_count):
        header = optional + optional_size + index * 40
        virtual_size, rva, raw_size, raw = unpack("IIII", header + 8)
        flags, = unpack("I", header + 36)
        sections.append((rva, virtual_size, raw_size, raw, flags))

    def file_offset(rva, length):
        for base, _, raw_size, raw, _ in sections:
            if base <= rva and rva + length <= base + raw_size:
                offset = raw + rva - base
                if offset + length <= len(data):
                    return offset
        raise ValueError(f"RVA 0x{rva:x} (+{length}) lies outside file-backed sections")

    table_offset = file_offset(table, size)
    previous_end = 0
    for index in range(size // 12):
        begin, end, unwind = unpack("III", table_offset + index * 12)
        if begin >= end:
            raise ValueError(f"empty or reversed runtime-function range at 0x{begin:x}")
        if begin < previous_end:
            raise ValueError(f"overlapping or unsorted runtime-function range at 0x{begin:x} "
                             f"(previous end 0x{previous_end:x})")
        if not any(base <= begin < end <= base + virtual_size and flags & 0x20000000
                   for base, virtual_size, _, _, flags in sections):
            raise ValueError(f"runtime-function range 0x{begin:x}..0x{end:x} is not executable")
        offset = file_offset(unwind, 4)
        version_flags, _, codes, _ = unpack("BBBB", offset)
        version = version_flags & 7
        if version not in (1, 2, 3):
            raise ValueError(f"invalid unwind-info version {version} at 0x{unwind:x}")
        # Version 3 has a different variable-length encoding. For versions
        # 1/2, also check the code slots and optional handler/chained record.
        if version in (1, 2):
            flags = version_flags >> 3
            trailer = 12 if flags & 4 else 4 if flags & 3 else 0
            file_offset(unwind, 4 + ((codes + 1) & ~1) * 2 + trailer)
        previous_end = end
    return size // 12


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: check_pe_unwind.py <x64.exe-or-dll> [...]")
    for argument in sys.argv[1:]:
        path = Path(argument)
        try:
            count = check(path.read_bytes())
        except (OSError, ValueError) as error:
            raise SystemExit(f"{path}: {error}") from error
        print(f"{path}: {count} ordered, nonoverlapping runtime-function records")


if __name__ == "__main__":
    main()
