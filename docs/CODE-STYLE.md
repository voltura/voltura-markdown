# Code style

The code should tell the story through names, small focused operations, and explicit ownership. Keep the native implementation direct: startup speed, predictable rendering, and a small standalone executable are design constraints.

## Automated formatting

- C++: **clang-format 22.1.3**, using the root `.clang-format` (LLVM base, four spaces, 110 columns).
- Python development tools: **Black 26.10.1**, configured in `pyproject.toml`.
- UTF-8, spaces, final newline, and LF line endings: `.editorconfig`.
- PowerShell and CMake: four-space indentation, one operation per statement, and readable argument lists.

Install the development-only Python formatter and run:

```powershell
python -m pip install -r requirements-dev.txt
.\scripts\format.ps1
.\scripts\format.ps1 -Check
```

The script locates clang-format on PATH or in Visual Studio's LLVM tools. Pass `-ClangFormat` for another installation. It formats our source, tests, and tools; it never reformats vendored upstream code. Neither formatter is part of the executable.

## Names and structure

Use names that expose purpose and units. Keep parsing, image loading, and shell integration in their existing modules; keep window lifecycle and drawing together until a cohesive boundary justifies extraction. Avoid speculative layers, repeated helpers, giant text layouts, UI-thread file/network I/O, and unrelated cleanup in bug fixes.

Use RAII for native resources where practical. Show cancellation, ownership transfer, bounds, and failure paths explicitly. Background results must be checked against the current document/view before use.

## Comments

Prefer clear code over narration. Comments are appropriate for a Windows API quirk, a performance invariant, ownership transfer, or a subtle reason a tempting simplification would be wrong. Explain **why**, not what the next statement does. Keep third-party notices intact. Do not add author/date banners, commented-out code, or a comment above every function. User-visible behavior belongs in the help and user guide; architecture belongs in the repository map.

## Review and verification

Review concrete failure paths and changed behavior. Add regression coverage for a confirmed bug; avoid tests that merely duplicate implementation details. Run the relevant native, UI, CommonMark, and package checks described in [Building](BUILDING.md). Review and tests reduce risk; neither proves that software has no bugs.
