#!/usr/bin/env python3
"""Rebuild the Windows multi-resolution icon from app.svg (requires Inkscape and Pillow)."""
import os
from pathlib import Path
import subprocess
import tempfile
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="openssh-manager-icon-") as directory:
    temporary = Path(directory)
    png = temporary / "render.png"
    environment = dict(os.environ, INKSCAPE_PROFILE_DIR=str(temporary / "inkscape"))
    subprocess.run(["inkscape", str(ROOT / "resources/app.svg"), "--export-type=png",
                    "--export-width=1024", f"--export-filename={png}"], env=environment, check=True)
    with Image.open(png) as source:
        source.convert("RGBA").save(ROOT / "resources/app.ico", format="ICO",
            sizes=[(s, s) for s in (16, 20, 24, 32, 40, 48, 64, 128, 256)])
print(ROOT / "resources/app.ico")
