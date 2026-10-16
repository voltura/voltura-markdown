# Architecture and code map

[Project overview](../README.md) · [Repository/document map](REPOSITORY.md) · [Build and checks](BUILDING.md)

The reader is a native Windows process with one window per invocation. Its priorities are fast first content, responsive input, a small executable, and a read-only document model. There is no browser engine or editable document widget tree.

## Source ownership

| File | Responsibility | Change it for |
| --- | --- | --- |
| [main.cpp](../src/main.cpp) | Application/window lifecycle, DirectWrite layouts, Direct2D canvas, selection/search, scrolling, themes, toolbar/menu, Help/About navigation | Reading interactions, controls, layout, painting, fullscreen, DPI |
| [document.h](../src/document.h) / [document.cpp](../src/document.cpp) | Encodings, MD4C adapter, formatted text, blocks/runs/cells, source segmentation, safe HTML subset | Syntax, text/anchor model, encoding handling, parser bounds |
| [images.h](../src/images.h) / [images.cpp](../src/images.cpp) | Background image queue/cache, local/HTTPS reads, WIC and NanoSVG decoding | Image formats, fetch/decode limits, caching |
| [shell.h](../src/shell.h) / [shell.cpp](../src/shell.cpp) | Preferences, links, registration, clipboard/file actions, external app chooser, license text | Windows integration and persisted options |
| [empty-state.h](../src/empty-state.h) / [empty-state.cpp](../src/empty-state.cpp) | Static native vector illustration | Empty-view artwork and matching icon source |
| [app.rc](../assets/app.rc) / [app.manifest](../assets/app.manifest) | Embedded icon, version metadata, platform/DPI declarations | Executable identity and Windows resources |
| `scripts/installer.nsi` / `scripts/build-installer.ps1` | NSIS packaging, per-user install/upgrade/uninstall integration | Optional Windows Setup distribution |
| [CMakeLists.txt](../CMakeLists.txt) | Targets, static runtime, system libraries, test registration | Build dependencies and packaging configuration |

`vm::Document` retains original decoded source, rendered text, flat blocks, path/encoding metadata, and timings. `Block` references text ranges with style/link `Run` entries and table `Cell` entries. The flat structure avoids a native window/widget per Markdown element.

## Document flow

```text
Command line / Open / drop
          ↓
App starts cancellable background load
          ↓
Read bytes → detect/decode encoding → MD4C callbacks
          ↓
Document { source, formatted text, blocks/runs/cells }
          ↓
Completion message to UI thread (discard obsolete work)
          ↓
Index block heights → lay out viewport and nearby blocks
          ↓
Bounded DirectWrite layouts → Direct2D document canvas
```

Parsing covers the complete document for references and nesting. Layout is progressive around the viewport. Very long source/code/paragraph text is split into bounded layout units; unusually long table cells use continuation rows. Parsed document/source memory still grows with input, independently of bounded layout and image caches.

Source view derives blocks from the original decoded source. Theme/view changes retain document structure and reuse or rebuild layout as appropriate. If formatting fails, the reader attempts to expose complete read-only source. Selection/copy never modifies source bytes or saves files.

## Independent UI and image paths

The toolbar/header and document canvas are separate windows. Native controls use offscreen buffering. Scrolling or image completion invalidates the document area; the loading spinner invalidates its own small region. Keep redraw regions narrow so document activity cannot cause repeated toolbar painting.

Visible/nearby image blocks request `vm::Images` entries. Background workers fetch/decode with bounded work and cache limits, then notify the UI. Text interaction does not wait for image network or decoding work. WIC handles PNG/JPEG/GIF, and NanoSVG handles a restricted static SVG subset.

Search also runs in the background and returns results through a window message. Opening a new file cancels obsolete work. Window destruction owns cleanup; do not leave workers targeting a destroyed window.

The shell boundary owns optional per-user registration, file clipboard data, supported URL launching, Explorer, and the external-editor chooser. Default application selection remains with Windows Settings. Preferences are small per-user local data; self-test modes avoid changing normal preferences.

## Change and verification map

| Change area | Relevant evidence |
| --- | --- |
| Parsing/encoding/source fidelity | [tests.cpp](../tests/tests.cpp), [CommonMark runner](../scripts/commonmark.py), [compatibility guide](COMPATIBILITY.md) |
| Images, registry, clipboard, Unicode paths | [integration.cpp](../tests/integration.cpp); registry tests use an isolated override |
| Toolbar, search, scrolling, themes, fullscreen, Help/About | `--verify-ui`, [showcase fixture](../tests/showcase.md), actual light/dark display inspection |
| Runtime/resources/icon/version | [package verifier](../scripts/verify-package.py), [artwork notes](../assets/ICON-NOTES.md), clean Windows execution |
| Startup/layout/memory | [benchmark runner](../scripts/benchmark.py), [performance methodology](../PERFORMANCE.md), actual cold/warm conditions |

See the [code style guide](CODE-STYLE.md) before editing. Keep new functionality in the existing owner when it remains cohesive; extract a dedicated module when complexity justifies it, without adding layers solely for file-count symmetry. Preserve the no-edit contract and measure performance-sensitive changes.
