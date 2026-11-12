"""Cold-start the installed Lua GUI with a private cache and audit its TeX inputs."""

import argparse
import gzip
import os
from pathlib import Path
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET


def recorded_inputs(recorder):
    cwd = recorder.parent
    paths = set()
    for line in recorder.read_text(encoding="utf-8", errors="replace").splitlines():
        kind, _, value = line.partition(" ")
        if kind == "PWD":
            cwd = Path(value.strip('"'))
        elif kind == "INPUT":
            paths.add((cwd / value.strip('"')).resolve())
    return paths


def verify(root, work):
    tree = root / "runtime" / "luatex"
    state = work / "state"
    if state.exists():
        raise RuntimeError(f"Cold start needs a fresh state directory: {state}")
    work.mkdir(parents=True, exist_ok=True)
    profile = work / "user"
    profile.mkdir(exist_ok=True)
    windows = Path(os.environ["SystemRoot"])
    env = {key: os.environ[key] for key in ("SystemRoot", "WINDIR", "SystemDrive", "COMSPEC") if key in os.environ}
    env.update({
        "PATH": str(windows / "System32"), "USERPROFILE": str(profile), "HOME": str(profile),
        "LOCALAPPDATA": str(profile), "APPDATA": str(profile), "TEMP": str(work), "TMP": str(work),
        "STEMTEX_LUA_SMOKE_STATE": str(state), "QT_FORCE_STDERR_LOGGING": "1",
    })
    # A system-wide TeX configuration or font path must not override the bundled tree.
    for key in ("TEXMFROOT", "TEXMFCNF", "TEXMFDIST", "TEXMFHOME", "TEXINPUTS", "LUAINPUTS", "OPENTYPEFONTS"):
        env[key] = str(work / "unavailable-tex-tree")
    gui = root / "gui"
    # In a single-engine install test the same automatic default as the Start menu shortcut.
    both = (root / "runtime/bin/windows/xetexdaemon.exe").is_file()
    select = ["--engine", "luatex"] if both else []
    for exe in ("stemtex-renderer-gui", "stemtex-profile-creator"):
        with (work / (exe + ".log")).open("wb") as log:
            completed = subprocess.run([gui / (exe + ".exe"), "--smoke", *select], cwd=work, env=env,
                                       stdout=log, stderr=subprocess.STDOUT, timeout=480)
        if completed.returncode:
            raise RuntimeError(f"{exe} returned {completed.returncode}. See {log.name}")

    inputs = set()
    for job in ("lualatex", "worker"):
        records = list(state.rglob(job + ".fls"))
        if not records:
            raise RuntimeError(f"No input record for {job}")
        for record in records:
            files = recorded_inputs(record)
            if not files:
                raise RuntimeError(f"Empty input record: {record}")
            for path in files:
                # luaotfload also opens Windows fonts while indexing the machine's font names.
                if path.is_relative_to(windows / "Fonts") and path.suffix.lower() in {".ttf", ".ttc", ".otf", ".otc"}:
                    continue
                if not (path.is_relative_to(tree) or path.is_relative_to(work) or path.is_relative_to(gui / "profiles")):
                    raise RuntimeError(f"Input outside the bundled tree and test state: {path}")
            inputs.update(files)
    if not any(path.name == "latex.ltx" for path in inputs):
        raise RuntimeError("Cold format creation did not record latex.ltx")
    if not any(p.stat().st_size for p in (state / "cache").rglob("lualatex.fmt")):
        raise RuntimeError("No new LuaLaTeX format in the private cache")
    fonts = set()
    for index in state.rglob("luaotfload-names.lua.gz"):
        with gzip.open(index, "rt", encoding="utf-8") as stream:
            fonts.update(Path(p).resolve() for p in re.findall(r'\["fullpath"\]\s*=\s*"([^"]+)"', stream.read()))
    for font in fonts:
        if any(part.lower() == "texmf-dist" for part in font.parts) and not font.is_relative_to(tree):
            raise RuntimeError(f"External TeX font in the new font index: {font}")
    for name in ("lmroman10-regular.otf", "latinmodern-math.otf"):
        if not any(p.name.lower() == name and p.is_relative_to(tree) for p in fonts & inputs):
            raise RuntimeError(f"Bundled font missing from the new index or input record: {name}")
    if not any(p.read_bytes().startswith(b"%PDF-") for p in state.rglob("*.pdf")):
        raise RuntimeError("No PDF from the installed worker")
    if not any(ET.parse(p).find(".//{http://www.w3.org/2000/svg}path") is not None for p in state.rglob("*.svg")):
        raise RuntimeError("No SVG glyph paths from the installed worker")
    print(f"PASS: cold LuaLaTeX format, PDF/SVG previews, Profile Creator, {len(inputs)} recorded inputs.")


def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument("install_root", type=Path)
    args.add_argument("--work-dir", type=Path, help="keep logs, recorder files and PDFs in this directory")
    options = args.parse_args()
    root = options.install_root.resolve()
    if options.work_dir:
        verify(root, options.work_dir.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix="stemtex-lua-") as directory:
            verify(root, Path(directory))


if __name__ == "__main__":
    main()
