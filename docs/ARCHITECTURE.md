# Architecture and code map

[Project overview](../README.md) · [Repository/document map](REPOSITORY.md) · [Build and checks](BUILDING.md)

The reader is a native Windows process with one window per invocation. Its priorities are fast first content, responsive input, and a small executable. Reading uses an immutable document model; optional source editing uses a separate draft and lazily loaded Windows Rich Edit control. There is no browser engine.

## Source ownership

| File | Responsibility | Change it for |
| --- | --- | --- |
| [main.cpp](../src/main.cpp) | Application/window lifecycle, DirectWrite layouts, Direct2D canvas, selection/search, scrolling, themes, toolbar/menu, Help/About navigation | Reading interactions, controls, layout, painting, fullscreen, DPI |
| [search.h](../src/search.h) | Shared ordinal case-insensitive search and constant-memory counting | Matching, wrapping, cancellation and total-count consistency |
| [document.h](../src/document.h) / [document.cpp](../src/document.cpp) | Encodings, MD4C adapter, formatted text, blocks/runs/cells, source segmentation, safe HTML subset | Syntax, text/anchor model, encoding handling, parser bounds |
| [edit.h](../src/edit.h) / [edit.cpp](../src/edit.cpp) | Markdown text operations, encoding-aware atomic saves, Markdown-aware validation | Insertions, source fidelity, conflict detection, warnings |
| [editor.h](../src/editor.h) / [editor.cpp](../src/editor.cpp) | Lazy native editor, toolbar/dialogs, draft state and background work | Editing interactions, undo, preview, validation scheduling |
| [editor-tools.h](../src/editor-tools.h) / [editor-tools.cpp](../src/editor-tools.cpp) | Outline, bounded syntax helpers, tables, replacements, recovery, managed assets, HTML and native print | On-demand editing tools; no reader startup work |
| [images.h](../src/images.h) / [images.cpp](../src/images.cpp) | Background image queue/cache, local/HTTPS reads, WIC and NanoSVG decoding | Image formats, fetch/decode limits, caching |
| [shell.h](../src/shell.h) / [shell.cpp](../src/shell.cpp) | Preferences, links, registration, clipboard/file actions, external app chooser, license text | Windows integration and persisted options |
| [empty-state.h](../src/empty-state.h) / [empty-state.cpp](../src/empty-state.cpp) | Static native vector illustration | Empty-view artwork and matching icon source |
| [app.rc](../assets/app.rc) / [app.manifest](../assets/app.manifest) | Embedded icon, version metadata, platform/DPI declarations | Executable identity and Windows resources |
| `scripts/installer.nsi` / `scripts/build-installer.ps1` | NSIS packaging, per-user install/upgrade/uninstall integration | Optional Windows Setup distribution |
| [CMakeLists.txt](../CMakeLists.txt) | Targets, static runtime, system libraries, test registration | Build dependencies and packaging configuration |

`vm::Document` retains original decoded source, rendered text, flat blocks, path/encoding metadata. Timing fields exist only in the instrumented test build. `Block` references text ranges with style/link `Run` entries and table `Cell` entries. Blocks use segmented storage to avoid whole-document reallocation peaks. Ordinary text has no style record; image, language and anchor metadata allocate only when present. The flat structure avoids a native window/widget per Markdown element.

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

Source view derives blocks lazily from the original decoded source. The initial scroll-height index is built in linear time; measured-height updates remain logarithmic. Theme/view changes retain document structure and reuse or rebuild layout as appropriate. If a block cannot be laid out, its text remains readable locally; parser failure retains the complete text in reading mode when memory permits. Selection/copy never modifies source bytes or saves files.

## Independent UI and image paths

The toolbar/header and document canvas are separate windows. Native controls use offscreen buffering. Scrolling or image completion invalidates the document area; the loading spinner invalidates its own small region. Keep redraw regions narrow so document activity cannot cause repeated toolbar painting.

Visible/nearby image blocks request `vm::Images` entries. Background workers fetch/decode with bounded work and cache limits, then notify the UI. Text interaction does not wait for image network or decoding work. WIC handles PNG/JPEG/GIF, and NanoSVG handles a restricted static SVG subset.

Search also runs in the background and returns results through a window message. After the first highlighted frame is presented, a separate lower-priority worker counts hits using constant memory. Navigation keeps the total and advances the current ordinal without restarting that count. Independent query and navigation generations reject obsolete results. Opening a new file cancels obsolete work. Window destruction owns cleanup; do not leave workers targeting a destroyed window.

The shell boundary owns optional per-user registration, file clipboard data, supported URL launching, Explorer, and the external-editor chooser. Default application selection remains with Windows Settings. Preferences are small per-user local data; the separate test executable can use temporary preferences. Production `vmd.exe` and its parser exclude test state, options, verification routines, failure injection and timing code with `VMD_TESTING` guards; `vmd-test.exe` and `markdown_test` explicitly enable those guards.

## Change and verification map

| Change area | Relevant evidence |
| --- | --- |
| Parsing/encoding/source fidelity | [tests.cpp](../tests/tests.cpp), [CommonMark runner](../scripts/commonmark.py), [compatibility guide](COMPATIBILITY.md) |
| Images, registry, clipboard, Unicode paths | [integration.cpp](../tests/integration.cpp); registry tests use an isolated override |
| Toolbar, search, scrolling, themes, fullscreen, Help/About | `vmd-test.exe --verify-ui`, [showcase fixture](../tests/showcase.md), actual light/dark display inspection |
| Runtime/resources/icon/version | [package verifier](../scripts/verify-package.py), [artwork notes](../assets/ICON-NOTES.md), clean Windows execution |
| Startup/layout/memory | [benchmark runner](../scripts/benchmark.py), [performance methodology](../PERFORMANCE.md), actual cold/warm conditions |

Editor initialization, parsing, validation, and saving do not run during reader startup. Typing marks a revision without copying the full source on each keystroke; a 400 ms timer captures a snapshot for cancellable background parsing and validation. Revision checks reject stale results. Preview reuses the renderer and leaves the native source/undo buffer alive. Optional live preview publishes completed draft revisions without changing typing focus, with side-by-side panes at 1000 logical pixels and stacked panes below that width. The renderer uses the preview pane width and maps pointer input from that pane; the main header retains the window width. Saving disables draft editing while a worker validates, checks the disk baseline, writes/flushed a sibling temporary file, rechecks the baseline, and replaces the destination. Abandoning a dirty session is centralized in the window lifecycle; update-driven restarts veto dirty drafts rather than storing draft text in restart snapshots.

Known native edits accumulate a changed interval, allowing snapshot capture to read only that interval. Undo, overtype, and uncertain mutations fall back to full capture. Visible source highlighting is bounded and suspends formatting undo records. Outline/fence maps come from background snapshots. Recovery has a separate periodic worker so continuous typing still checkpoints; retired recovery writers are drained before normal close. Image copying, replacement, export, and external-change probes use revision-checked background jobs. Print cancellation uses CancelDC with synchronized HDC ownership. None of these workers run during reader startup.

See the [code style guide](CODE-STYLE.md) before editing. Keep new functionality in the existing owner when it remains cohesive; extract a dedicated module when complexity justifies it, without adding layers solely for file-count symmetry. Preserve read-mode behavior and measure performance-sensitive changes.
