# Changelog

## 1.1.0 — 2026-10-17

- Faster first-page display using software rendering while hardware rendering prepares in the background.
- Preserves reading position and selection when hardware rendering takes over.
- Continues displaying the document in software if hardware setup fails.

## 1.0.0 — 2026-10-17

- Reduced memory use and opening time for documents with many Markdown blocks.
- Correct initial control scaling and added DPI-transition regression coverage.

## 1.0.0 Preview 2 — 2026-10-17

- Timestamped, self-signed portable reader and Windows installer.
- Local certificate setup, automatic release signing, and signature verification.

## 1.0.0 Preview — 2026-10-16

- Portable, single-file Windows Markdown reader and optional per-user Setup.
- Formatted/source views, selection, copying, search, zoom, and smooth scrolling.
- Floating lower-right themed zoom controls with percentage, reset, a brief fade, and scrollbar clearance.
- Left/Right horizontal scrolling.
- CommonMark parsing with tables, task lists, footnotes, alerts, and local/HTTPS images.
- Common embedded HTML, themed keyboard keys, and readable local fallbacks within formatted view.
- Light/dark themes for the reader, menus, tooltips, scrollbars, Help, and About.
- Fullscreen startup, direct-close X, and direct Escape exit when launched with a file path.
- Copy file/name/path, show in Explorer, and choose an external editor.
- Optional default-reader setup through Windows Settings.
- Native document-and-lightning artwork and multi-resolution app icon.
- Background loading, bounded layout/image caches, Unicode boundary handling, and cancellable search.
- MIT-licensed application, documentation, and current artwork.

See [release notes](docs/releases/1.1.0.md) for downloads and [compatibility](docs/COMPATIBILITY.md) for limits.
