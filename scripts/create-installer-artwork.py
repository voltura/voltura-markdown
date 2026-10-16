"""Compose the MUI2 welcome/finish banner from the existing native app artwork.

Development-only Pillow; no AI redraw. Run from any directory on Windows.
Outputs the 1312x2512 24-bit BMP and a PNG preview. Uses Segoe UI to match Windows
and the Voltura Books installer layout, without reading any sibling repository.
For the sharpest icon export, first run the native export-icon helper, then pass
--vector-render build/icon-vector-render.png. Without that explicit argument the
checked-in 1024px icon master is used, without relying on transient build files.
"""

from pathlib import Path
import argparse
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
WIDTH, HEIGHT, SCALE, OUTPUT_SCALE = 656, 1256, 4, 2


def font(size, bold=False):
    name = "segoeuib.ttf" if bold else "segoeui.ttf"
    path = Path(os.environ.get("SystemRoot", "C:/Windows")) / "Fonts" / name
    if not path.is_file():
        raise FileNotFoundError(f"Segoe UI font required to reproduce artwork: {path}")
    return ImageFont.truetype(str(path), round(size * SCALE))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vector-render", type=Path, help="2048px transparent native renderer export")
    args = parser.parse_args()
    image = Image.new("RGB", (WIDTH * SCALE, HEIGHT * SCALE))
    draw = ImageDraw.Draw(image)
    top, bottom = (12, 23, 39), (28, 40, 77)
    for y in range(HEIGHT * SCALE):
        t = y / (HEIGHT * SCALE - 1)
        color = tuple(round(a + (b - a) * t) for a, b in zip(top, bottom))
        draw.line((0, y, WIDTH * SCALE, y), fill=color)

    def line(points, fill, width=1):
        draw.line(tuple(round(value * SCALE) for value in points), fill=fill, width=round(width * SCALE))

    # Sister-app motif: restrained parallel diagonals at the upper right.
    for x in range(403, 710, 30):
        line((x, 0, x + 240, 284), (24, 48, 68), 1)

    draw.text((66 * SCALE, 83 * SCALE), "VOLTURA", font=font(46, True), fill=(177, 211, 241))
    draw.text((64 * SCALE, 153 * SCALE), "MARKDOWN", font=font(66, True), fill=(247, 250, 255))

    # Short cyan-to-violet brand rule echoes the bolt badge in the illustration.
    for x in range(88 * SCALE):
        t = x / (88 * SCALE - 1)
        color = tuple(round(a + (b - a) * t) for a, b in zip((115, 226, 255), (115, 111, 251)))
        draw.line((68 * SCALE + x, 271 * SCALE, 68 * SCALE + x, 278 * SCALE), fill=color)
    draw.ellipse((65 * SCALE, 271 * SCALE, 72 * SCALE, 278 * SCALE), fill=(115, 226, 255))
    draw.ellipse((152 * SCALE, 271 * SCALE, 159 * SCALE, 278 * SCALE), fill=(115, 111, 251))

    # Preserve the exact approved icon: dark paper and the cyan lightning badge.
    icon_source = args.vector_render if args.vector_render else ASSETS / "icon-master.png"
    with Image.open(icon_source) as source:
        art = source.convert("RGBA")
        if args.vector_render:
            # Match the approved master composition exactly, retaining native high-res edges.
            if min(art.size) < 2048:
                raise ValueError("Native vector render must be at least 2048 pixels")
            bounds = art.getchannel("A").getbbox()
            if bounds is None:
                raise ValueError("Native vector render has no visible artwork")
            art = art.crop(bounds)
            factor = 2048 * 0.88 / max(art.size)
            art = art.resize(tuple(round(d * factor) for d in art.size), Image.Resampling.LANCZOS)
            canvas = Image.new("RGBA", (2048, 2048))
            canvas.alpha_composite(art, ((2048 - art.width) // 2, (2048 - art.height) // 2))
            art = canvas
        art = art.resize((620 * SCALE, 620 * SCALE), Image.Resampling.LANCZOS)
        image.paste(art, (18 * SCALE, 322 * SCALE), art)

    draw = ImageDraw.Draw(image)
    draw.text((66 * SCALE, 1030 * SCALE), "Fast reading.", font=font(40), fill=(238, 246, 255))
    draw.text((66 * SCALE, 1084 * SCALE), "One small app.", font=font(40), fill=(177, 211, 241))

    # Work at 4x design resolution, then export 2x: no low-resolution text upscaling.
    output_size = (WIDTH * OUTPUT_SCALE, HEIGHT * OUTPUT_SCALE)
    image = image.resize(output_size, Image.Resampling.LANCZOS)
    image.save(ASSETS / "installer-banner.bmp", format="BMP")
    image.save(ASSETS / "installer-preview.png", format="PNG")
    with Image.open(ASSETS / "installer-banner.bmp") as result:
        assert result.size == output_size and result.mode == "RGB"
    raw = (ASSETS / "installer-banner.bmp").read_bytes()
    assert int.from_bytes(raw[28:30], "little") == 24
    print(f"Verified {output_size[0]}x{output_size[1]} RGB / 24-bit BMP: {len(raw):,} bytes")
    print(f"Icon source: {icon_source}")


if __name__ == "__main__":
    main()
