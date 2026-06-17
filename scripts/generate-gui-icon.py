from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

from PIL import Image


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def find_inkscape() -> Path | None:
    candidates: list[Path] = []
    if os.environ.get("INKSCAPE"):
        candidates.append(Path(os.environ["INKSCAPE"]))
    candidates.extend([
        Path(r"C:\Program Files\Inkscape\bin\inkscape.exe"),
        Path(r"C:\Program Files\Inkscape\inkscape.exe"),
        Path(r"C:\Program Files (x86)\Inkscape\bin\inkscape.exe"),
        Path(r"C:\msys64\ucrt64\bin\inkscape.exe"),
        Path(r"C:\msys64\mingw64\bin\inkscape.exe"),
    ])
    for candidate in candidates:
        if candidate and candidate.exists():
            return candidate
    return None


def render_svg(svg: Path, png: Path, size: int) -> None:
    try:
        import cairosvg  # type: ignore

        cairosvg.svg2png(
            url=str(svg),
            write_to=str(png),
            output_width=size,
            output_height=size,
        )
        return
    except Exception:
        pass

    inkscape = find_inkscape()
    if not inkscape:
        raise RuntimeError("No SVG renderer found. Install Inkscape or provide Python cairosvg.")

    subprocess.run(
        [
            str(inkscape),
            str(svg),
            "--export-type=png",
            f"--export-filename={png}",
            f"--export-width={size}",
            f"--export-height={size}",
        ],
        check=True,
    )


def main() -> int:
    root = repo_root()
    assets = root / "gui" / "assets"
    svg = assets / "stemtex-renderer-gui.svg"
    png256 = assets / "stemtex-renderer-gui-256.png"
    ico = assets / "stemtex-renderer-gui.ico"

    if not svg.exists():
        print(f"missing SVG: {svg}", file=sys.stderr)
        return 1

    render_svg(svg, png256, 256)

    base = Image.open(png256).convert("RGBA")
    sizes = [16, 24, 32, 48, 64, 128, 256]
    base.save(ico, format="ICO", sizes=[(size, size) for size in sizes])

    print(f"wrote {png256}")
    print(f"wrote {ico}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
