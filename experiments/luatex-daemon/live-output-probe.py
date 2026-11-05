"""Stock LuaLaTeX stdin/shipout probe; does not implement checkpoint recovery."""
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build" / "luatex-live-output-probe"
TEX = r"""\documentclass{article}
\usepackage{unicode-math}
\setmainfont{lmroman10-regular.otf}
\setmathfont{latinmodern-math.otf}
\newbox\probebox
\newcount\requestnumber
\def\signal#1{\directlua{io.stdout:write(string.char(10).."#1"..string.char(10)); io.stdout:flush()}}
\begin{document}
\setbox\probebox=\hbox{$x^2+\frac{1}{2}$}
\signal{PROBE_READY}
\loop
  \advance\requestnumber by 1
  \signal{PROBE_WAIT:\the\requestnumber}
  \read16 to\requestfile
  \begingroup
    \setbox\probebox=\hbox{\input{\requestfile}}
    \signal{PROBE_BOX:\the\wd\probebox,\the\ht\probebox,\the\dp\probebox}
    \shipout\copy\probebox
  \endgroup
  \signal{PROBE_DONE:\the\requestnumber}
\ifnum\requestnumber<3\repeat
\signal{PROBE_AWAIT_STOP}
\read16 to\stopline
\end{document}
"""


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "worker.tex").write_text(TEX, encoding="utf-8")
    for index, body in enumerate([r"$x^2+\frac{1}{2}$", r"$\sum_{k=1}^{n}k^2$", r"$x^2+\frac{1}{2}$"], 1):
        (OUT / f"request{index}.tex").write_text(body + "\n", encoding="utf-8")
    env = os.environ.copy()
    # Keep generated Lua font caches inside the experiment directory.
    (OUT / "cache").mkdir(exist_ok=True)
    env["TEXMFCACHE"] = str(OUT / "cache")
    env["TEXMFVAR"] = str(OUT / "cache")
    engine = shutil.which("lualatex")
    if not engine:
        raise RuntimeError("lualatex is required")
    started = time.perf_counter()
    process = subprocess.Popen(
        [engine, "--interaction=errorstopmode", "--halt-on-error", "--no-shell-escape", "worker.tex"],
        cwd=OUT, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
    )
    lines = queue.Queue()
    transcript = []

    def collect():
        for line in process.stdout:
            transcript.append(line)
            lines.put(line.strip())
        lines.put(None)

    reader = threading.Thread(target=collect, daemon=True)
    reader.start()

    def until(marker):
        deadline = time.monotonic() + 60
        observed = []
        while True:
            line = lines.get(timeout=max(0.01, deadline - time.monotonic()))
            if line is None:
                raise RuntimeError("Engine exited before " + marker + "\n" + "".join(transcript[-20:]))
            observed.append(line)
            if line == marker:
                return observed
            if time.monotonic() >= deadline:
                raise TimeoutError(marker)

    def send(value):
        process.stdin.write(value + "\n")
        process.stdin.flush()

    def inspect_pdf():
        pdf = OUT / "worker.pdf"
        data = pdf.read_bytes() if pdf.exists() else b""
        return {"bytes": len(data), "has_startxref": b"startxref" in data, "has_eof": b"%%EOF" in data}

    try:
        until("PROBE_READY")
        report = {"engine": engine, "pid": process.pid, "ready_ms": (time.perf_counter() - started) * 1000, "requests": []}
        for index in range(1, 4):
            until(f"PROBE_WAIT:{index}")
            sent = time.perf_counter()
            send(f"request{index}.tex")
            observed = until(f"PROBE_DONE:{index}")
            box = next(line for line in observed if line.startswith("PROBE_BOX:"))
            report["requests"].append({"id": index, "elapsed_ms": (time.perf_counter() - sent) * 1000, "box": box, "pdf": inspect_pdf()})
            assert process.poll() is None, "Worker must remain alive"
        until("PROBE_AWAIT_STOP")
        report["before_stop"] = inspect_pdf()
        (OUT / "before-stop.pdf").write_bytes((OUT / "worker.pdf").read_bytes())
        assert report["requests"][0]["box"] == report["requests"][2]["box"], "Repeated layout drift"
        send("stop")
        assert process.wait(timeout=30) == 0, "Engine failed at shutdown"
        reader.join(timeout=5)
        report["after_stop"] = inspect_pdf()
        assert report["after_stop"]["has_eof"] and report["after_stop"]["has_startxref"]
        (OUT / "result.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(json.dumps(report, indent=2))
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=10)
        reader.join(timeout=5)
        (OUT / "console.log").write_text("".join(transcript), encoding="utf-8")
        process.stdin.close()
        process.stdout.close()


if __name__ == "__main__":
    main()
