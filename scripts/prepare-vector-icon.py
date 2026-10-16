"""Crop/pad the native renderer's transparent export, archive originals, and package ICO.

Run after build/export-icon.exe build/icon-vector-render.png.
Requires development-only Pillow. No raster redraw or font substitution is performed.
"""

from pathlib import Path
from shutil import copy2
import subprocess
import sys

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"


def main():
    for current, original in (
        ("icon-master.png", "icon-master-original.png"),
        ("app.ico", "app-original.ico"),
    ):
        if not (ASSETS / original).exists():
            copy2(ASSETS / current, ASSETS / original)

    with Image.open(ROOT / "build/icon-vector-render.png") as render:
        image = render.convert("RGBA")
        bbox = image.getchannel("A").getbbox()
        if bbox is None:
            raise ValueError("Native renderer produced no visible artwork")
        image = image.crop(bbox)
        size = 1024
        # Preserve all silhouette/shadow pixels, with 6% breathing room on each side.
        factor = size * 0.88 / max(image.size)
        image = image.resize(tuple(round(d * factor) for d in image.size), Image.Resampling.LANCZOS)
        master = Image.new("RGBA", (size, size))
        master.alpha_composite(image, ((size - image.width) // 2, (size - image.height) // 2))
        master.save(ASSETS / "icon-master.png")

    subprocess.run([sys.executable, str(ROOT / "scripts/package-icon.py")], check=True)

    sizes = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
    preview = Image.new("RGB", (1100, 640), "white")
    draw = ImageDraw.Draw(preview)
    with Image.open(ASSETS / "app.ico") as icon:
        for row, bg in enumerate(((16, 19, 25), (250, 251, 253))):
            y = row * 320
            draw.rectangle((0, y, 1100, y + 320), fill=bg)
            draw.text((20, y + 15), "Native vector icon / exact-size Windows frames", fill=(140, 155, 180))
            x = 20
            for side in sizes:
                frame = icon.ico.getimage((side, side)).convert("RGBA")
                preview.paste(frame, (x, y + 55 + (256 - side) // 2), frame)
                draw.text((x, y + 45), str(side), fill=(140, 155, 180))
                x += max(side, 40) + 18
    preview.save(ROOT / "build/icon-vector-qa.png")
    print("Master: 1024x1024 RGBA; original artwork archived without modification")
    print("QA: build/icon-vector-qa.png")


if __name__ == "__main__":
    main()
