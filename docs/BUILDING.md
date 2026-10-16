# Build and verify

Install Visual Studio 2026 with **Desktop development with C++**, the Windows SDK, and CMake tools. No network access is required to build: parser and SVG sources are vendored at pinned revisions.

```powershell
.\scripts\build.ps1
python scripts/commonmark.py
python scripts/verify-package.py
```

The build script compiles the x64 Release build, runs CTest, and copies the standalone executable to `dist/vmd.exe`. Build intermediates and test executables in `build/Release` are not distribution files. Python is needed only for optional verification/benchmark tooling. Pillow is needed only to regenerate or inspect icon artwork.

```powershell
python scripts/benchmark.py
.\build\Release\vmd.exe "$PWD\tests\showcase.md" --verify-ui "$PWD\build\ui-checks.jsonl" --exit-after 20000
```

Benchmarks write generated documents and JSON measurements under `build/bench`. UI verification uses a temporary in-process preference state and leaves saved preferences unchanged. See [Performance](../PERFORMANCE.md) for measurements and coverage.

## Toolchain details

`scripts/build.ps1` discovers Visual Studio through `vswhere`, then configures CMake with the **Visual Studio 18 2026** generator. Install that version and its C++ desktop/CMake components; the script does not currently provide an older-generator fallback. Build on Windows x64. The CMake project requires CMake 3.24 or later, and the selected generator also needs a CMake version that supports Visual Studio 2026.

The source tree can build offline. Python verification tools use the standard library; Pillow is an optional development dependency for icon packaging and screen capture. Nothing from Python or Pillow is distributed with the reader.

## Verification scope

CTest covers the adapter and native integrations. `commonmark.py` checks the pinned 652-example fixture against parser output and adapter ranges; it does not prove every renderer feature is visually identical to a browser. The UI harness runs inside the native app with temporary preferences. Inspect actual light/dark rendering, fullscreen controls, selection, About, and mixed-DPI behavior separately.

Use unique log filenames because UI logs append. Generated captures, logs, benchmark fixtures, and intermediates belong under `build/`, which is ignored. Avoid including private documents in fixtures or reports.

For icon regeneration, follow [artwork notes](../assets/ICON-NOTES.md). For publishing gates, follow [release procedure](RELEASING.md).

## Formatting and source navigation

Follow the [code style guide](CODE-STYLE.md) for the pinned formatting tools and `scripts/format.ps1` checks. The [architecture guide](ARCHITECTURE.md) maps responsibilities and test coverage; the [repository guide](REPOSITORY.md) maps folders and documents.

## Optional Windows installer

Install NSIS 3.x for packaging. The installer uses Modern UI 2 and solid LZMA compression; it wraps the same statically linked reader rather than adding a runtime dependency.

```powershell
.\scripts\build-installer.ps1
# Reuse the current verified Release build:
.\scripts\build-installer.ps1 -SkipBuild
```

The output is `dist/VolturaMarkdown-Setup-1.0.0-win-x64.exe`. Building the installer does not install it. `-SkipBuild` is for an already built and verified `dist/vmd.exe`; do not use it with stale output. Installer source lives in `scripts/installer.nsi`, with its banner under `assets/installer-banner.bmp`.

Setup is per-user, adds Start menu/Installed apps integration, and installs the reader plus application/third-party notices. The portable download remains one executable. The default-reader finish option is unchecked; selecting it registers the installed app and opens Windows Default apps for the final user choice.

The installer lifecycle smoke test builds a separate test identity, installs and upgrades it under `build/`, then uninstalls it. It temporarily creates its own per-user Installed apps entry and Start menu shortcut. It never registers Markdown associations. It checks binary identity, notices, and retention of documents/preferences:

```powershell
.\scripts\test-installer.ps1
```

See [Compatibility](COMPATIBILITY.md#platform) for outstanding environment checks.
