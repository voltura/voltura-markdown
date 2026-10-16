# Voltura Markdown icon

## Current artwork

- `icon-master.png`: 1024 x 1024 transparent RGBA master exported from the native Direct2D empty-state illustration in `src/empty-state.cpp`. It uses the exact dark-paper artwork: layered folded document, reading lines, blue/violet badge, cyan lightning bolt. No AI redraw, fonts, glyphs, or external artwork were used for this replacement.
- Rendered at 2048 x 2048 with the software Direct2D/WIC renderer, cropped to the complete alpha bounds including its subtle shadow, then padded to fill 88% of the square canvas height. The result stays recognizable on light and dark Windows surfaces.
- `app.ico`: 10 PNG-compressed Windows icon frames at 16, 20, 24, 32, 40, 48, 64, 96, 128, and 256 pixels, all 32-bit RGBA. The 256 entry uses the standard zero dimension fields.
- The existing `assets/app.rc` embeds the ICO. Export tools are development-only: no Pillow, image file, or additional DLL is required beside the application executable.

## Reproduce

From a Visual Studio x64 Native Tools shell in the repository root:

```bat
cl /nologo /EHsc /std:c++20 /DUNICODE /D_UNICODE /DNOMINMAX /MT scripts\export-icon.cpp src\empty-state.cpp /Fe:build\export-icon.exe /Fo:build\ /link d2d1.lib windowscodecs.lib ole32.lib
build\export-icon.exe build\icon-vector-render.png
python scripts/prepare-vector-icon.py
```

The final command uses development-only Pillow to crop/pad the master and invokes `scripts/package-icon.py` to generate and verify the ICO. To only repackage an existing master, run `python scripts/package-icon.py`.

## Verification

The packager checks all ten frame dimensions, payload bounds, RGBA depth, and transparency. Inspect exact-size frames on light/dark surfaces. The package verifier checks embedded executable icon sizes.
