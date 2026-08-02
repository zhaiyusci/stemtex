#!/usr/bin/env python3
"""Audit the StemTeX in-process XeTeX checkpoint mapping.

This is a guardrail, not the checkpoint implementation. The canonical inventory
is still XeTeX's storefmtfile()/loadfmtfile() path; this script checks that the
format-style user-state arrays and scalars have an intentional in-process
snapshot mapping or a documented exception.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class MappingCheck:
    name: str
    source_patterns: tuple[str, ...]
    save_patterns: tuple[str, ...]
    restore_patterns: tuple[str, ...]


FORMAT_ARRAY_FIELDS = {
    "mem",
    "eqtb",
    "hash",
    "strstart",
    "strpool",
    "fontinfo",
    "fontcheck",
    "fontsize",
    "fontdsize",
    "fontparams",
    "hyphenchar",
    "skewchar",
    "fontname",
    "fontarea",
    "fontbc",
    "fontec",
    "charbase",
    "widthbase",
    "heightbase",
    "depthbase",
    "italicbase",
    "ligkernbase",
    "kernbase",
    "extenbase",
    "parambase",
    "fontglue",
    "bcharlabel",
    "fontbchar",
    "fontfalsebchar",
    "hyphword",
    "hyphlink",
    "hyphlist",
    "trietrl",
    "trietro",
    "trietrc",
    "hyfdistance",
    "hyfnum",
    "hyfnext",
    "trieused",
}

INTENTIONAL_FORMAT_ARRAY_EXCEPTIONS = {
    # Format file metadata, not TeX user state.
    "formatengine",
    # Primitive table is fixed after the format has loaded. User definitions
    # live in eqtb/hash, which the checkpoint snapshots.
    "prim",
}

CHECKS: tuple[MappingCheck, ...] = (
    MappingCheck(
        "string pool",
        (r"dumpthings\s*\(\s*strstart\s*\[", r"dumpthings\s*\(\s*strpool\s*\["),
        (r"strstart_copy", r"strpool_copy", r"poolptr_copy", r"strptr_copy"),
        (r"memcpy\s*\(\s*strstart", r"memcpy\s*\(\s*strpool", r"poolptr\s*=", r"strptr\s*="),
    ),
    MappingCheck(
        "main memory",
        (r"dumpthings\s*\(\s*mem\s*\[",),
        (r"stemtex_snapshot_save\s*\(\s*&stemtex_checkpoint\.yzmem_copy", r"lomemmax_copy", r"himemmin_copy"),
        (r"stemtex_snapshot_restore\s*\(\s*&stemtex_checkpoint\.yzmem_copy", r"lomemmax\s*=", r"himemmin\s*="),
    ),
    MappingCheck(
        "eqtb",
        (r"dumpthings\s*\(\s*eqtb\s*\[",),
        (r"stemtex_snapshot_save\s*\(\s*&stemtex_checkpoint\.zeqtb_copy",),
        (r"stemtex_snapshot_restore\s*\(\s*&stemtex_checkpoint\.zeqtb_copy",),
    ),
    MappingCheck(
        "hash table",
        (r"dumpthings\s*\(\s*hash\s*\[", r"dumpint\s*\(\s*hashused\s*\)", r"dumpint\s*\(\s*cscount\s*\)"),
        (r"stemtex_snapshot_save\s*\(\s*&stemtex_checkpoint\.yhash_copy", r"hashused_copy", r"cscount_copy"),
        (r"stemtex_snapshot_restore\s*\(\s*&stemtex_checkpoint\.yhash_copy", r"hashused\s*=", r"cscount\s*="),
    ),
    MappingCheck(
        "e-TeX sparse roots",
        (r"dumpint\s*\(\s*saroot\s*\[\s*k\s*\]\s*\)",),
        (r"memcpy\s*\(\s*stemtex_checkpoint\.saroot_copy\s*,\s*saroot\s*,\s*sizeof\s*\(\s*halfword\s*\)\s*\*\s*7",),
        (r"memcpy\s*\(\s*saroot\s*,\s*stemtex_checkpoint\.saroot_copy\s*,\s*sizeof\s*\(\s*halfword\s*\)\s*\*\s*7",),
    ),
    MappingCheck(
        "font info",
        (r"dumpthings\s*\(\s*fontinfo\s*\[", r"dumpint\s*\(\s*fmemptr\s*\)", r"dumpint\s*\(\s*fontptr\s*\)"),
        (r"stemtex_snapshot_save\s*\(\s*&stemtex_checkpoint\.fontinfo_copy", r"fmemptr_copy", r"fontptr_copy"),
        (r"stemtex_snapshot_restore\s*\(\s*&stemtex_checkpoint\.fontinfo_copy", r"fmemptr\s*=", r"fontptr\s*="),
    ),
    MappingCheck(
        "font arrays",
        (
            r"dumpthings\s*\(\s*fontcheck\s*\[",
            r"dumpthings\s*\(\s*fontsize\s*\[",
            r"dumpthings\s*\(\s*fontdsize\s*\[",
            r"dumpthings\s*\(\s*fontparams\s*\[",
            r"dumpthings\s*\(\s*hyphenchar\s*\[",
            r"dumpthings\s*\(\s*skewchar\s*\[",
            r"dumpthings\s*\(\s*fontname\s*\[",
            r"dumpthings\s*\(\s*fontarea\s*\[",
            r"dumpthings\s*\(\s*fontbc\s*\[",
            r"dumpthings\s*\(\s*fontec\s*\[",
            r"dumpthings\s*\(\s*charbase\s*\[",
            r"dumpthings\s*\(\s*widthbase\s*\[",
            r"dumpthings\s*\(\s*heightbase\s*\[",
            r"dumpthings\s*\(\s*depthbase\s*\[",
            r"dumpthings\s*\(\s*italicbase\s*\[",
            r"dumpthings\s*\(\s*ligkernbase\s*\[",
            r"dumpthings\s*\(\s*kernbase\s*\[",
            r"dumpthings\s*\(\s*extenbase\s*\[",
            r"dumpthings\s*\(\s*parambase\s*\[",
            r"dumpthings\s*\(\s*fontglue\s*\[",
            r"dumpthings\s*\(\s*bcharlabel\s*\[",
            r"dumpthings\s*\(\s*fontbchar\s*\[",
            r"dumpthings\s*\(\s*fontfalsebchar\s*\[",
        ),
        (
            r"STEMTEX_SAVE_ARRAY\s*\(\s*fontcheck",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*fontsize",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*fontdsize",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*fontparams",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*hyphenchar",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*skewchar",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*fontname",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*fontarea",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*fontbc",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*fontec",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*charbase",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*widthbase",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*heightbase",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*depthbase",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*italicbase",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*ligkernbase",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*kernbase",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*extenbase",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*parambase",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*fontglue",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*bcharlabel",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*fontbchar",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*fontfalsebchar",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*fontused",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*fontlayoutengine",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*fontmapping",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*fontflags",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*fontletterspace",
        ),
        (
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*fontcheck",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*fontsize",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*fontdsize",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*fontparams",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*hyphenchar",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*skewchar",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*fontname",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*fontarea",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*fontbc",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*fontec",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*charbase",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*widthbase",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*heightbase",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*depthbase",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*italicbase",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*ligkernbase",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*kernbase",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*extenbase",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*parambase",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*fontglue",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*bcharlabel",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*fontbchar",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*fontfalsebchar",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*fontused",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*fontlayoutengine",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*fontmapping",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*fontflags",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*fontletterspace",
        ),
    ),
    MappingCheck(
        "font rollback resources",
        (r"fontmapping\s*\[k\s*\]\s*!=\s*0",),
        (r"stemtex_release_rolled_back_user_resources", r"releasefontengine", r"TECkit_DisposeConverter"),
        (r"stemtex_release_rolled_back_user_resources",),
    ),
    MappingCheck(
        "hyphen exceptions",
        (r"hyphword\s*\[k\s*\]", r"hyphlink\s*\[k\s*\]", r"hyphlist\s*\[k\s*\]"),
        (r"hyphcount_copy", r"hyphnext_copy", r"STEMTEX_SAVE_ARRAY\s*\(\s*hyphword", r"STEMTEX_SAVE_ARRAY\s*\(\s*hyphlist", r"STEMTEX_SAVE_ARRAY\s*\(\s*hyphlink"),
        (r"hyphcount\s*=", r"hyphnext\s*=", r"STEMTEX_RESTORE_ARRAY\s*\(\s*hyphword", r"STEMTEX_RESTORE_ARRAY\s*\(\s*hyphlist", r"STEMTEX_RESTORE_ARRAY\s*\(\s*hyphlink"),
    ),
    MappingCheck(
        "trie tables",
        (
            r"dumpthings\s*\(\s*trietrl\s*\[",
            r"dumpthings\s*\(\s*trietro\s*\[",
            r"dumpthings\s*\(\s*trietrc\s*\[",
            r"dumpthings\s*\(\s*hyfdistance\s*\[",
            r"dumpthings\s*\(\s*hyfnum\s*\[",
            r"dumpthings\s*\(\s*hyfnext\s*\[",
            r"dumpint\s*\(\s*hyphstart\s*\)",
            r"dumpint\s*\(\s*maxhyphchar\s*\)",
            r"dumpint\s*\(\s*trieopptr\s*\)",
            r"dumpint\s*\(\s*trieused\s*\[\s*k\s*\]\s*\)",
        ),
        (
            r"triemax_copy",
            r"hyphstart_copy",
            r"maxhyphchar_copy",
            r"trieopptr_copy",
            r"trienotready_copy",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*trietrl",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*trietro",
            r"STEMTEX_SAVE_ARRAY\s*\(\s*trietrc",
            r"hyfdistance_copy",
            r"hyfnum_copy",
            r"hyfnext_copy",
            r"opstart_copy",
            r"trieused_copy",
        ),
        (
            r"triemax\s*=",
            r"hyphstart\s*=",
            r"maxhyphchar\s*=",
            r"trieopptr\s*=",
            r"trienotready\s*=",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*trietrl",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*trietro",
            r"STEMTEX_RESTORE_ARRAY\s*\(\s*trietrc",
            r"memcpy\s*\(\s*hyfdistance",
            r"memcpy\s*\(\s*hyfnum",
            r"memcpy\s*\(\s*hyfnext",
            r"memcpy\s*\(\s*opstart",
            r"memcpy\s*\(\s*trieused",
        ),
    ),
)


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", source)
    if not match:
        raise ValueError(f"function {name} not found")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise ValueError(f"function {name} body is incomplete")


def matches(text: str, pattern: str) -> bool:
    return re.search(pattern, text, re.MULTILINE | re.DOTALL) is not None


def missing_patterns(label: str, text: str, patterns: tuple[str, ...]) -> list[str]:
    return [pattern for pattern in patterns if not matches(text, pattern)]


def store_array_fields(store_body: str) -> set[str]:
    fields = set(re.findall(r"\bdumpthings\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[|,|\))", store_body))
    fields.update(re.findall(r"\bdumphh\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\[", store_body))
    for field in ("hyphword", "hyphlink", "hyphlist", "trieused"):
        if re.search(rf"\b{field}\s*\[", store_body):
            fields.add(field)
    return fields


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    repo = args.repo.resolve()
    xetexini = read_text(repo / "texlive-xetex" / "src" / "web2c" / "xetexini.c")
    xetex0 = read_text(repo / "texlive-xetex" / "src" / "web2c" / "xetex0.c")

    store = function_body(xetexini, "storefmtfile")
    save_user = function_body(xetex0, "stemtex_save_tex_user_state")
    release_resources = function_body(xetex0, "stemtex_release_rolled_back_user_resources")
    restore_user = function_body(xetex0, "stemtex_restore_tex_user_state")
    save_checkpoint = function_body(xetex0, "stemtex_save_checkpoint")
    restore_checkpoint = function_body(xetex0, "stemtex_restore_checkpoint")
    checkpoint_source = "\n".join([save_user, release_resources, restore_user, save_checkpoint, restore_checkpoint])

    failures: list[str] = []

    source_arrays = store_array_fields(store)
    unknown_arrays = source_arrays - FORMAT_ARRAY_FIELDS - INTENTIONAL_FORMAT_ARRAY_EXCEPTIONS
    missing_known_arrays = FORMAT_ARRAY_FIELDS - source_arrays
    if unknown_arrays:
        failures.append("storefmtfile has unclassified dump arrays: " + ", ".join(sorted(unknown_arrays)))
    if missing_known_arrays:
        failures.append("expected format dump arrays not found: " + ", ".join(sorted(missing_known_arrays)))

    for check in CHECKS:
        for label, text, patterns in (
            ("source", store, check.source_patterns),
            ("save", checkpoint_source, check.save_patterns),
            ("restore", checkpoint_source, check.restore_patterns),
        ):
            missing = missing_patterns(label, text, patterns)
            if missing:
                failures.append(f"{check.name}: missing {label} pattern(s): " + "; ".join(missing))

    if failures:
        print("StemTeX checkpoint audit failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    if args.verbose:
        print("StemTeX checkpoint audit passed.")
        print("format arrays:", ", ".join(sorted(source_arrays)))
    else:
        print("StemTeX checkpoint audit passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
