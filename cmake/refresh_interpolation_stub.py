"""Preserve an official SDK stub's exports and add upstream animation imports.

Produces a symbol list for upstream symgen, never a runtime implementation.
Supports the thin 64-bit ELF/Mach-O stubs distributed by the pinned SDK.
"""
import argparse
from pathlib import Path
import struct


def cstring(data, offset):
    return data[offset:data.index(0, offset)].decode("utf-8")


def exports(data):
    if data[:4] == b"\x7fELF":
        if data[4:6] != b"\x02\x01":
            raise ValueError("Expected a little-endian ELF64 SDK stub")
        offset = struct.unpack_from("<Q", data, 40)[0]
        size, count = struct.unpack_from("<HH", data, 58)
        sections = [struct.unpack_from("<IIQQQQIIQQ", data, offset + i * size)
                    for i in range(count)]
        names = set()
        for section in sections:
            if section[1] != 11:  # SHT_DYNSYM
                continue
            strings_section = sections[section[6]]
            strings = data[strings_section[4]:strings_section[4] + strings_section[5]]
            for pos in range(section[4], section[4] + section[5], section[9]):
                name, info, other, shndx, _, _ = struct.unpack_from("<IBBHQQ", data, pos)
                if name and shndx and info >> 4 in (1, 2) and other & 3 in (0, 3):
                    names.add(cstring(strings, name))
        return "elf", names
    if data[:4] == b"\xcf\xfa\xed\xfe":
        commands = struct.unpack_from("<I", data, 16)[0]
        offset = 32
        names = set()
        for _ in range(commands):
            command, size = struct.unpack_from("<II", data, offset)
            if command == 2:  # LC_SYMTAB
                symoff, count, stroff, strsize = struct.unpack_from("<IIII", data, offset + 8)
                strings = data[stroff:stroff + strsize]
                for i in range(count):
                    name, kind, _, _, _ = struct.unpack_from("<IBBHQ", data, symoff + i * 16)
                    if name and kind & 1 and not kind & 0xe0 and kind & 0x0e:
                        names.add(cstring(strings, name))
            offset += size
        return "macho", names
    raise ValueError("Unrecognized SDK stub format")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--symbols", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verify", type=Path)
    args = parser.parse_args()
    kind, names = exports(args.input.read_bytes())
    if not names:
        raise ValueError("SDK stub has no readable exports; refusing to replace it")
    additions = {line.strip() for line in args.symbols.read_text().splitlines()
                 if line.strip() and not line.startswith("#")}
    if kind == "macho":
        additions = {"_" + name for name in additions}
    original_count = len(names)
    names.update(additions)
    if args.verify:
        generated_kind, generated_names = exports(args.verify.read_bytes())
        missing = names - generated_names
        if generated_kind != kind or missing:
            raise ValueError(f"Regenerated SDK stub lost exports: {sorted(missing)}")
        print(f"Verified all {len(names)} required {kind} exports")
        return
    args.output.write_text("\n".join(sorted(names)) + "\n", encoding="utf-8")
    print(f"Preserved {original_count} {kind} exports; added {len(names) - original_count} upstream imports")


if __name__ == "__main__":
    main()
