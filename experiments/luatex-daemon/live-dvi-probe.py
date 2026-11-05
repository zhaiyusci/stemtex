"""Inspect stock or patched LuaTeX DVI while its worker is still alive.

Only complete parsed pages are copied and finalized. No bytes from the final
engine output are used to repair a live snapshot. This is not a renderer backend.
"""
import argparse
import json
import os
from pathlib import Path
import queue
import shutil
import struct
import subprocess
import threading
import time
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build" / "luatex-live-dvi-probe"
LOOP = r"""
\newbox\probebox
\newcount\requestnumber
\def\signal#1{\directlua{io.stdout:write(string.char(10).."#1"..string.char(10)); io.stdout:flush()}}
\signal{PROBE_READY}
\loop
  \advance\requestnumber by 1
  \signal{PROBE_WAIT:\the\requestnumber}
  \read16 to\requestfile
  \begingroup
    \setbox\probebox=\vbox{\input{\requestfile}}
    \shipout\box\probebox
  \endgroup
  \signal{PROBE_DONE:\the\requestnumber}
\ifnum\requestnumber<@REQUEST_COUNT@\repeat
\signal{PROBE_AWAIT_STOP}
\read16 to\stopline
"""


def parse_dvi(data):
    """Parse standard DVI commands, stopping safely at an incomplete command."""
    pages, fonts = [], {}
    pos = depth = max_depth = 0
    bop = None
    has_post = False
    while pos < len(data):
        start = pos
        op = data[pos]
        pos += 1
        extra = 0
        if op <= 127 or op in (138, 140, 141, 142, 147, 152, 161, 166) or 171 <= op <= 234:
            pass
        elif 128 <= op <= 131:
            extra = op - 127
        elif op in (132, 137):
            extra = 8
        elif 133 <= op <= 136:
            extra = op - 132
        elif op == 139:
            extra = 44
        elif 143 <= op <= 146:
            extra = op - 142
        elif 148 <= op <= 151:
            extra = op - 147
        elif 153 <= op <= 156:
            extra = op - 152
        elif 157 <= op <= 160:
            extra = op - 156
        elif 162 <= op <= 165:
            extra = op - 161
        elif 167 <= op <= 170:
            extra = op - 166
        elif 235 <= op <= 238:
            extra = op - 234
        elif 239 <= op <= 242:
            size = op - 238
            if pos + size > len(data):
                break
            extra = size + int.from_bytes(data[pos:pos + size], "big")
        elif 243 <= op <= 246:
            size = op - 242
            if pos + size + 14 > len(data):
                break
            extra = size + 14 + data[pos + size + 12] + data[pos + size + 13]
        elif op == 247:
            if pos + 14 > len(data):
                break
            extra = 14 + data[pos + 13]
        elif op == 248:
            has_post = True
            break
        elif op == 255:  # direction extension, not used by these probes
            extra = 1
        else:
            raise ValueError(f"Unsupported DVI opcode {op} at {start}")
        pos += extra
        if pos > len(data):
            break
        if op == 139:
            assert bop is None
            bop, depth = start, 0
        elif op == 141:
            depth += 1
            max_depth = max(max_depth, depth)
        elif op == 142:
            depth -= 1
            assert depth >= 0
        elif op == 140:
            assert bop is not None and depth == 0
            pages.append({"bop": bop, "end": pos, "fonts": dict(fonts), "max_depth": max_depth})
            bop = None
        elif 243 <= op <= 246:
            fonts[int.from_bytes(data[start + 1:start + 1 + op - 242], "big")] = data[start:pos]
    return pages, has_post


def finalize(data, pages):
    last = pages[-1]
    result = bytearray(data[:last["end"]])
    post_offset = len(result)
    # Conservative bounds suffice for this probe; page content determines layout.
    result.extend(bytes([248]) + struct.pack(">i", last["bop"]) + data[2:14])
    result.extend(struct.pack(">IIHH", 0x3FFFFFFF, 0x3FFFFFFF, last["max_depth"], len(pages)))
    # Match LuaTeX's descending font-ID postamble order. This also keeps
    # dvisvgm's choice of base-size glyph paths stable across snapshots.
    for font_id in sorted(last["fonts"], reverse=True):
        result.extend(last["fonts"][font_id])
    result.extend(bytes([249]) + struct.pack(">iB", post_offset, data[1]) + bytes([223]) * 4)
    while len(result) % 4:
        result.append(223)
    return result


def verify_final_dvi(data, expected_pages):
    """Check the actual file tail and absolute BOP/postamble offsets."""
    assert len(data) % 4 == 0, f"DVI length {len(data)} is not a multiple of four"
    assert len(data) >= 15 and data[0] == 247, "Invalid DVI preamble"
    tail = len(data)
    while tail and data[tail - 1] == 223:
        tail -= 1
    padding = len(data) - tail
    assert padding >= 4, f"Only {padding} trailing DVI filler bytes"
    post_post = tail - 6
    assert post_post >= 0 and data[post_post] == 249, "Missing post_post before filler"
    assert data[post_post + 5] == data[1], "Preamble/post_post format IDs differ"
    post = int.from_bytes(data[post_post + 1:post_post + 5], "big", signed=True)
    assert 0 <= post < post_post - 28 and data[post] == 248, "Invalid post_post pointer"
    assert data[post + 5:post + 17] == data[2:14], "Preamble/postamble units differ"
    count = int.from_bytes(data[post + 27:post + 29], "big")
    assert count == expected_pages, f"Postamble says {count} pages, expected {expected_pages}"
    pages, has_post = parse_dvi(data)
    assert has_post and len(pages) == count, "Parsed page count differs from postamble"
    assert pages and pages[-1]["end"] <= post, "Postamble overlaps page data"
    last_bop = int.from_bytes(data[post + 1:post + 5], "big", signed=True)
    assert last_bop == pages[-1]["bop"], "Postamble does not point at the last BOP"
    previous = -1
    for page in pages:
        bop = page["bop"]
        back = int.from_bytes(data[bop + 41:bop + 45], "big", signed=True)
        assert back == previous, f"BOP {bop} points to {back}, expected {previous}"
        previous = bop
    declared_depth = int.from_bytes(data[post + 25:post + 27], "big")
    assert declared_depth >= pages[-1]["max_depth"], "Postamble underreports DVI stack depth"
    return {"length_multiple_of_four": True, "trailing_filler_bytes": padding,
            "post_post_offset": post_post, "postamble_offset": post,
            "bop_chain_pages": count, "page_end_residues": sorted({p["end"] % 4 for p in pages})}


def convert(path, page):
    """Convert exactly one page, with fresh outputs and strict exit checking."""
    results = {}
    output_base = path.with_name(f"{path.stem}-page-{page}")
    for tool, args in [
        ("dvipdfmx", ["-q", "-s", str(page), "-o", str(output_base.with_suffix(".pdf")), str(path)]),
        ("dvisvgm", ["--no-fonts", f"--page={page}", "--output=" + str(output_base.with_suffix(".svg")), str(path)]),
    ]:
        executable = shutil.which(tool)
        if not executable:
            raise RuntimeError(f"Required converter {tool} is not on PATH")
        output_file = output_base.with_suffix(".pdf" if tool == "dvipdfmx" else ".svg")
        output_file.unlink(missing_ok=True)
        done = subprocess.run([executable, *args], cwd=path.parent, capture_output=True,
                              text=True, encoding="utf-8", errors="replace", timeout=30)
        output = done.stdout + done.stderr
        log = output_base.with_suffix("." + tool + ".log")
        log.write_text(output, encoding="utf-8")
        if done.returncode != 0 or not output_file.exists() or output_file.stat().st_size == 0:
            raise RuntimeError(f"{tool} failed for page {page}, exit {done.returncode}: {log}\n{output[-1400:]}")
        results[tool] = {"exit": done.returncode, "output": str(output_file), "tail": output[-1400:]}
    return results


def svg_geometry(path):
    """Ignore font-definition ordering/IDs; compare actual path uses and positions."""
    root = ET.parse(path).getroot()
    definitions = {element.attrib["id"]: element for element in root.iter() if "id" in element.attrib}

    def geometry(element):
        attributes = dict(element.attrib)
        attributes.pop("id", None)
        reference = attributes.pop("{http://www.w3.org/1999/xlink}href", None)
        if reference:
            attributes["referenced_geometry"] = geometry(definitions[reference[1:]])
        return (element.tag, tuple(sorted(attributes.items())), element.text.strip() if element.text else "",
                tuple(geometry(child) for child in element if not child.tag.endswith("}defs")))

    return geometry(root)


def run_case(name, engine_name, row_counts, unicode, options, run_dir):
    work = run_dir / name
    work.mkdir(parents=True, exist_ok=True)
    if unicode:
        preamble = r"""\documentclass{article}
\usepackage{unicode-math}
\setmainfont{lmroman10-regular.otf}
\setmathfont{latinmodern-math.otf}
\begin{document}
"""
        ending = r"\end{document}"
        line = r"\hbox{Request @INDEX@: Hello $x^2+\frac{1}{2}+\sum_{k=1}^{n}k^2$}"
    else:
        preamble, ending = r"\font\probe=cmr10\probe", r"\bye"
        line = r"\hbox{Request @INDEX@: Hello DVI streaming 0123456789 abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ}"
    loop = LOOP.replace("@REQUEST_COUNT@", str(len(row_counts)))
    (work / "worker.tex").write_text(preamble + "\n" + loop + ending + "\n", encoding="utf-8")
    for index, rows in enumerate(row_counts, 1):
        body = (line.replace("@INDEX@", str(index)) + "\n") * rows
        if name.endswith("-alignment"):
            # Four pages each with zero/one extra trailing whitespace byte.
            # One of the two constant-size runs advances by an odd number of
            # bytes, covering all page-end residues while retaining valid output.
            padding = r"\space" * ((index - 1) // 4)
            body += r"\special{papersize=595pt,842pt" + padding + "}\n"
        (work / f"request{index}.tex").write_text(body, encoding="utf-8")
    env = os.environ.copy()
    cache = ROOT / "build" / "luatex-live-output-probe" / "cache"
    cache.mkdir(parents=True, exist_ok=True)
    env.setdefault("TEXMFCACHE", str(cache))
    env.setdefault("TEXMFVAR", str(cache))
    executable = shutil.which(engine_name)
    if not executable:
        raise RuntimeError(f"Missing {engine_name}")
    program = "lualatex" if unicode else "luatex"
    format_file = options.latex_format if unicode else options.plain_format
    command = [executable, f"--fmt={format_file}", f"--progname={program}",
               "--output-format=dvi", "--interaction=errorstopmode", "--halt-on-error", "--no-shell-escape"]
    if options.flush_output_on_shipout:
        command.append("--flush-output-on-shipout")
    command.append("worker.tex")
    process = subprocess.Popen(command,
                               cwd=work, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    lines, transcript = queue.Queue(), []

    def collect():
        for line in process.stdout:
            transcript.append(line)
            lines.put(line.strip())
        lines.put(None)

    reader = threading.Thread(target=collect, daemon=True)
    reader.start()

    def until(marker):
        deadline = time.monotonic() + 60
        while True:
            line = lines.get(timeout=max(0.01, deadline - time.monotonic()))
            if line is None:
                raise RuntimeError("Engine ended before " + marker + "\n" + "".join(transcript[-15:]))
            if line == marker:
                return
            if time.monotonic() >= deadline:
                raise TimeoutError(marker)

    def send(value):
        process.stdin.write(value + "\n")
        process.stdin.flush()

    def snapshot(label, live, expected_pages):
        path = work / "worker.dvi"
        data = path.read_bytes() if path.exists() else b""
        copied = work / (label + ".dvi")
        copied.write_bytes(data)
        pages, post = parse_dvi(data)
        result = {"bytes": len(data), "complete_pages": len(pages), "postamble": post}
        if live and options.flush_output_on_shipout:
            if len(pages) != expected_pages:
                raise AssertionError(f"shipout {expected_pages}: expected {expected_pages} immediately readable pages, got {len(pages)} ({len(data)} bytes)")
            if post or len(data) != pages[-1]["end"]:
                raise AssertionError(f"shipout {expected_pages}: live stream must end exactly after EOP")
        if pages and live:
            repaired = work / (label + "-finalized.dvi")
            repaired_data = finalize(data, pages)
            result["finalized_structure"] = verify_final_dvi(repaired_data, len(pages))
            repaired.write_bytes(repaired_data)
            result["selected_page"] = len(pages)
            result["conversion"] = convert(repaired, len(pages))
            assert process.poll() is None, "Worker ended during live conversion"
        return result

    report = {"case": name, "engine": executable, "command": command, "pid": process.pid,
              "work_directory": str(work), "flush_output_on_shipout": options.flush_output_on_shipout,
              "rows_per_page": row_counts, "live": []}
    try:
        until("PROBE_READY")
        for index in range(1, len(row_counts) + 1):
            until(f"PROBE_WAIT:{index}")
            send(f"request{index}.tex")
            until(f"PROBE_DONE:{index}")
            assert process.poll() is None
            report["live"].append(snapshot(f"live-{index}", True, index))
        until("PROBE_AWAIT_STOP")
        send("stop")
        assert process.wait(timeout=30) == 0
        report["final"] = snapshot("finished", False, len(row_counts))
        assert report["final"]["complete_pages"] == len(row_counts), "Final page count mismatch"
        assert report["final"]["postamble"], "Final postamble missing"
        final_data = (work / "finished.dvi").read_bytes()
        report["final"]["structure"] = verify_final_dvi(final_data, len(row_counts))
        if name.endswith("-alignment"):
            assert report["final"]["structure"]["page_end_residues"] == [0, 1, 2, 3], "Alignment corpus did not exercise all four page-end offsets"
        baseline_pages = sorted({live["selected_page"] for live in report["live"] if "selected_page" in live} | {len(row_counts)})
        report["final"]["conversions"] = {str(page): convert(work / "finished.dvi", page) for page in baseline_pages}
        for index, live in enumerate(report["live"], 1):
            if live["complete_pages"]:
                data = (work / f"live-{index}.dvi").read_bytes()
                pages, _ = parse_dvi(data)
                end = pages[-1]["end"]
                live["complete_prefix_matches_final"] = data[:end] == final_data[:end]
                assert live["complete_prefix_matches_final"], f"Live {index} complete bytes changed after shutdown"
                page = live["selected_page"]
                live_svg = Path(live["conversion"]["dvisvgm"]["output"])
                final_svg = Path(report["final"]["conversions"][str(page)]["dvisvgm"]["output"])
                live["selected_page_svg_geometry_matches_final"] = svg_geometry(live_svg) == svg_geometry(final_svg)
                assert live["selected_page_svg_geometry_matches_final"], f"Page {page} SVG geometry differs from final output"
    except Exception as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=10)
        reader.join(timeout=5)
        (work / "console.log").write_text("".join(transcript), encoding="utf-8")
        process.stdin.close()
        process.stdout.close()
    (work / "result.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2), flush=True)
    return report


if __name__ == "__main__":
    cases = {
        "plain-small": (False, [1, 1, 1]),
        "plain-large": (False, [300, 300, 300]),
        "unicode-small": (True, [1, 1, 1]),
        "unicode-large": (True, [300, 300, 300]),
        # Exercise small pages after partial/full buffer swaps repeatedly.
        "plain-boundary": (False, [1, 60, 1, 130, 1, 300, 1]),
        "unicode-boundary": (True, [1, 50, 1, 110, 1, 300, 1]),
        "plain-alignment": (False, [1] * 8),
        "unicode-alignment": (True, [1] * 8),
    }
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--luatex", default="luatex", help="plain LuaTeX executable (may be renamed)")
    parser.add_argument("--lualatex", default="lualatex", help="LuaHBTeX/LuaLaTeX executable (may be renamed)")
    parser.add_argument("--plain-format", default="luatex", help="plain format name or path, built with the tested engine version")
    parser.add_argument("--latex-format", default="lualatex", help="LaTeX format name or path, built with the tested engine version")
    parser.add_argument("--flush-output-on-shipout", action="store_true", help="pass the patched switch and require every page immediately on disk")
    parser.add_argument("--case", action="append", choices=cases, help="repeat to select cases; default is the original four cases")
    parser.add_argument("--output-directory", type=Path, default=OUT)
    options = parser.parse_args()
    output_root = options.output_directory.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    # Every run has fresh sources, engine files, and conversions. Never accept
    # last run's output if startup or a converter fails; preserve failure evidence.
    run_dir = Path(tempfile.mkdtemp(prefix="run-", dir=output_root))
    reports = []
    selected = options.case or list(cases)[:4]
    for name in selected:
        unicode, row_counts = cases[name]
        try:
            reports.append(run_case(name, options.lualatex if unicode else options.luatex,
                                    row_counts, unicode, options, run_dir))
        except Exception as exc:
            reports.append({"case": name, "error": f"{type(exc).__name__}: {exc}"})
    summary = json.dumps(reports, indent=2)
    (run_dir / "results.json").write_text(summary, encoding="utf-8")
    (output_root / "results.json").write_text(summary, encoding="utf-8")
    if any("error" in report for report in reports):
        raise SystemExit(1)
