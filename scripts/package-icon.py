"""Package the generated artwork as the multi-resolution Windows app icon.

Requires Pillow: python -m pip install Pillow
Run from any directory: python scripts/package-icon.py
The lossless high-resolution master is never modified.
"""

from pathlib import Path
from struct import unpack_from

from PIL import Image

SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
ROOT = Path(__file__).resolve().parents[1]
MASTER = ROOT / "assets" / "icon-master.png"
OUTPUT = ROOT / "assets" / "app.ico"


def main():
    with Image.open(MASTER) as source:
        if source.width != source.height or source.width < 1024:
            raise ValueError("The master must be square and at least 1024 pixels.")
        if source.mode != "RGBA" or source.getextrema()[3][0] != 0:
            raise ValueError("The master must have an RGBA transparent exterior.")
        # Pillow uses Lanczos for ICO resampling and PNG compression per frame.
        source.save(OUTPUT, format="ICO", sizes=[(s, s) for s in SIZES])

    raw = OUTPUT.read_bytes()
    reserved, kind, count = unpack_from("<HHH", raw)
    assert (reserved, kind, count) == (0, 1, len(SIZES))
    dimensions = []
    for index in range(count):
        width, height, _, _, planes, bits, length, offset = unpack_from("<BBBBHHII", raw, 6 + 16 * index)
        dimensions.append((width or 256, height or 256))
        assert planes == 0 and bits == 32
        assert offset + length <= len(raw)
    assert dimensions == [(s, s) for s in SIZES]
    with Image.open(OUTPUT) as packaged:
        for size in SIZES:
            frame = packaged.ico.getimage((size, size))
            assert frame.size == (size, size)
            assert frame.mode == "RGBA"
            assert frame.getextrema()[3][0] == 0
    print(f"Verified {OUTPUT}: {count} RGBA frames, {len(raw):,} bytes")
    print("Sizes: " + ", ".join(f"{s}x{s}" for s in SIZES))


if __name__ == "__main__":
    main()
