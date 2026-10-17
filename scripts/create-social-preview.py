"""Rebuild the website's 1200x630 sharing card with Pillow and Segoe UI.

Uses the approved app icon without redrawing it. Development only; the website
build copies the checked-in PNG and does not require Python or Pillow.
"""

from pathlib import Path
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
SCALE = 2
WIDTH, HEIGHT = 1200, 630


def font(size, bold=False):
    path = Path(os.environ.get("SystemRoot", "C:/Windows")) / "Fonts"
    return ImageFont.truetype(str(path / ("segoeuib.ttf" if bold else "segoeui.ttf")), size * SCALE)


def main():
    image = Image.new("RGB", (WIDTH * SCALE, HEIGHT * SCALE))
    draw = ImageDraw.Draw(image)
    for y in range(HEIGHT * SCALE):
        for x in range(WIDTH * SCALE):
            glow = max(0, 1 - (((x / SCALE - 965) / 650) ** 2 + ((y / SCALE - 275) / 470) ** 2))
            image.putpixel((x, y), tuple(round(a + b * glow) for a, b in ((12, 17), (23, 22), (39, 47))))

    def text(x, y, value, size, color, bold=False):
        draw.text((x * SCALE, y * SCALE), value, font=font(size, bold), fill=color)

    def line(points, color, width=1):
        draw.line(tuple(round(v * SCALE) for v in points), fill=color, width=width * SCALE)

    for x in range(780, 1270, 36):
        line((x, 0, x + 240, 240), "#20334b")

    text(70, 57, "VOLTURA MARKDOWN", 23, "#b1d3f1", True)
    line((72, 123, 140, 123), "#73e2ff", 5)
    line((140, 123, 175, 123), "#8783ff", 5)
    text(66, 170, "Markdown reader", 65, "#f7faff", True)
    text(66, 247, "for Windows.", 65, "#8edfff", True)
    text(71, 353, "Your notes. Your docs. Instantly readable.", 25, "#b7c8df")

    with Image.open(ROOT / "assets/icon-master.png") as source:
        icon = source.convert("RGBA")
        icon.thumbnail((430 * SCALE, 430 * SCALE), Image.Resampling.LANCZOS)
        image.paste(icon, (745 * SCALE, 102 * SCALE), icon)

    line((72, 470, 1128, 470), "#32435d")
    text(72, 503, "Free  ·  Windows 10 / 11  ·  Portable or installed", 23, "#d5e3f4")
    text(72, 553, "markdown.voltura.se", 21, "#8edfff")
    output = ROOT / "site/assets/social-preview.png"
    output.parent.mkdir(parents=True, exist_ok=True)
    image.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS).save(output, optimize=True)
    print(f"Created {output} ({WIDTH}x{HEIGHT})")


if __name__ == "__main__":
    main()
