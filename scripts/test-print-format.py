"""Inspect print-regression PDF geometry and fonts (requires pdfplumber)."""

import pathlib
import sys

import pdfplumber

path = pathlib.Path(sys.argv[1])
failures = []
with pdfplumber.open(path) as pdf:
    words = [word for page in pdf.pages for word in page.extract_words(extra_attrs=["fontname", "size"])]

    def token(name):
        return next(word for word in words if word["text"].startswith(name))

    base, mono, sup, sub, small = [
        token(name) for name in ("BaseToken", "MonoToken", "SuperToken", "SubToken", "SmallToken")
    ]
    if not any(
        abs(image["width"] - 30) < 0.5 and abs(image["height"] - 15) < 0.5
        for page in pdf.pages
        for image in page.images
    ):
        failures.append("023 printed images preserve explicit HTML dimensions")
    if not (
        mono["fontname"] != base["fontname"]
        and sup["bottom"] < base["bottom"] - 1
        and sub["top"] > base["top"] + 1
        and small["size"] < base["size"] - 1
    ):
        failures.append("024 printed inline code, superscript and small text retain their styles")
    bold, italic, code, normal = [
        token(name) for name in ("TableBold", "TableItalic", "TableCode", "NormalCell")
    ]
    if not all(word["fontname"] != normal["fontname"] for word in (bold, italic, code)):
        failures.append("025 printed table cells preserve inline formatting")
    header, data = token("HeaderToken"), token("DataToken")
    if header["fontname"] == data["fontname"] or data["fontname"] != normal["fontname"]:
        failures.append("Mixed HTML table rows must bold only their header cells")
    pdf.pages[0].to_image(resolution=120).save(path.with_name("format-preview.png"))

for failure in failures:
    print("FAIL:", failure)
if failures:
    raise SystemExit(1)
print("PASS: image dimensions, inline styles, and table formatting in", path)
