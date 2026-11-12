#!/usr/bin/env python3
"""Bind checkpoint memory/allocation domains to one unstripped MinGW engine.

Only input sections from this build's objects and static libraries are included.
The MinGW/CRT runtime and checkpoint implementation retain their live state.
The companion file is checked against the entire executable before use.
"""

import argparse
import json
import os
from pathlib import Path
import re
import struct


def pe_sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("Expected a PE executable")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    if struct.unpack_from("<H", data, optional)[0] != 0x20B:
        raise ValueError("Expected PE32+ (64-bit)")
    image_base = struct.unpack_from("<Q", data, optional + 24)[0]
    sections = []
    for index in range(count):
        offset = optional + optional_size + index * 40
        name = data[offset:offset + 8].split(b"\0", 1)[0].decode("ascii")
        size, rva = struct.unpack_from("<II", data, offset + 8)
        flags = struct.unpack_from("<I", data, offset + 36)[0]
        sections.append((name, rva, size, flags))
    return image_base, sections


def normalized_path(value, base):
    value = value.replace("\\", "/")
    # GNU tools can print either drive paths or MSYS drive mount paths.
    value = re.sub(r"^/([A-Za-z])/", lambda m: m[1] + ":/", value)
    path = Path(value)
    return (path if path.is_absolute() else base / path).resolve()


def merged(ranges):
    result = []
    for start, size in sorted(ranges):
        if result and start <= result[-1][0] + result[-1][1]:
            old, length = result[-1]
            result[-1] = (old, max(old + length, start + size) - old)
        else:
            result.append((start, size))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--map", required=True, type=Path)
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    binary = args.exe.read_bytes()
    image_base, sections = pe_sections(binary)
    work_root = args.work_root.resolve()
    output = args.output or Path(str(args.exe) + ".stemtex-checkpoint")
    ranges = {"C": [], "D": []}
    inventory = {}
    excluded = set()
    section_name = None
    # Input section records are indented. Output sections start in column zero.
    record = re.compile(r"^\s+(?:(\.[^\s]+|COMMON)\s+)?(0x[\da-fA-F]+)\s+(0x[\da-fA-F]+)\s+(.+?)\s*$")
    only_name = re.compile(r"^\s+(\.[^\s]+|COMMON)\s*$")
    map_text = args.map.read_text(encoding="utf-8", errors="replace")
    marker = "Linker script and memory map"
    if marker not in map_text:
        raise ValueError("Expected a GNU linker memory map")
    # The earlier discarded-input list also contains section-shaped records.
    for line in map_text.split(marker, 1)[1].splitlines():
        named = only_name.match(line)
        if named:
            section_name = named[1]
            continue
        entry = record.match(line)
        if not entry:
            section_name = None
            continue
        name = entry[1] or section_name
        section_name = None
        if name is None:
            continue
        kind = "C" if name == ".text" or name.startswith(".text$") or name.startswith(".text.") else (
            "D" if name in (".data", ".bss", "COMMON") or name.startswith((".data$", ".bss$", ".data.", ".bss.")) else None)
        if not kind:
            continue
        address, size = int(entry[2], 16), int(entry[3], 16)
        if not size:
            continue
        owner = entry[4]
        file_name = owner.split("(", 1)[0]
        if not file_name.endswith((".o", ".a")):
            continue
        path = normalized_path(file_name, args.map.resolve().parent)
        if not path.is_relative_to(work_root) or path.name == "stemtex-checkpoint.o":
            excluded.add(owner)
            continue
        rva = address - image_base
        containing = [s for s in sections if s[1] <= rva and rva + size <= s[1] + s[2]]
        if len(containing) != 1:
            raise ValueError(f"Input section outside PE image: {name} {owner}")
        pe_name, _, _, flags = containing[0]
        if kind == "C" and not flags & 0x20000000:
            raise ValueError(f"Non-executable allocation domain: {owner}")
        if kind == "D" and (not flags & 0x80000000 or pe_name not in (".data", ".bss")):
            raise ValueError(f"Unsafe global section: {pe_name} {owner}")
        ranges[kind].append((rva, size))
        counts = inventory.setdefault(owner, {"codeBytes": 0, "dataBytes": 0})
        counts["codeBytes" if kind == "C" else "dataBytes"] += size
    if not ranges["C"] or not ranges["D"]:
        raise ValueError("Link map did not supply both engine data and allocation domains")
    if not any("luatex" in owner for owner in inventory) or not any("lua53" in owner for owner in inventory):
        raise ValueError("Link map is missing the TeX or Lua VM ownership domain")
    checksum = 14695981039346656037
    for byte in binary:
        checksum = ((checksum ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    lines = ["STEMTEX_CHECKPOINT_RANGES_V1", f"{len(binary)} {checksum:016x}"]
    for kind in ("D", "C"):
        ranges[kind] = merged(ranges[kind])
        lines.extend(f"{kind} {rva:x} {size:x}" for rva, size in ranges[kind])
    temporary = output.with_name(output.name + ".tmp")
    temporary.write_text("\n".join(lines) + "\n", encoding="ascii")
    os.replace(temporary, output)
    audit = {"executable": str(args.exe.resolve()), "bytes": len(binary), "fnv1a64": f"{checksum:016x}",
             "dataBytes": sum(s for _, s in ranges["D"]), "codeBytes": sum(s for _, s in ranges["C"]),
             "includedObjects": inventory, "excludedRuntimeObjects": sorted(excluded)}
    Path(str(output) + ".json").write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"Checkpoint domains: {audit['dataBytes']} data bytes, {len(inventory)} engine/library objects")


if __name__ == "__main__":
    main()
