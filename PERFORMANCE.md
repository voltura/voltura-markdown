# Performance and verification

Local x64 MSVC Release measurements, 2026-10-16. Hardware: Intel Core Ultra 9 185H, 22 logical processors, 63.5 GiB usable RAM; Windows 11 Pro build 26300, 3840×2160 at 225% scaling.

## Startup and memory

Three separate launches per generated document, with ordinary Windows filesystem caching:

| Input | Median first content | Median read + parse | Peak parser working set |
| --- | ---: | ---: | ---: |
| 1 KiB | 134 ms | 0.32 ms | 3.6 MiB |
| 1 MiB | 144 ms | 38.8 ms | 21.6 MiB |
| 10 MiB | 549 ms | 563 ms | 199.5 MiB |
| 100 MiB stress | 3.98 s | 3.87 s | 2.12 GiB |

First-content and parsing measurements use separate processes. First-content timing begins near application entry and excludes OS loader time; cold-cache startup was not isolated. The 10 MiB fixture contains about 364,000 blocks, with first-content runs ranging from 383 to 765 ms. These are workload-specific measurements, not guarantees.

Pathological parsing: a 10 MiB unbroken paragraph took 81 ms; a 10 MiB fenced-code line 47 ms; a table with a 1 MiB cell 13 ms. Long content is segmented for layout. Parsed source/structure memory grows with input even though layout/image caches are bounded.

Regenerate results with `python scripts/benchmark.py`; output goes to ignored `build/bench/`.

## Rendering

The document canvas is separate from the buffered toolbar. Viewport layouts and narrow invalidation limit redraw work; scrolling honors Windows' reduced-motion setting.

Sample UI runs measured 2.14 ms mean / 3.88 ms p95 document paint time for the showcase and 0.75 ms / 1.12 ms for a 10 MiB fixture. Toolbar paint counters stayed unchanged during scrolling/loading; a focused-search capture series contained 12 identical toolbar frames. CPU paint timings and sampled captures do not establish sustained display-frame delivery.

## Automated coverage

- 652/652 CommonMark 0.31.2 examples: normalized parser HTML and adapter bounds, not pixel-identical renderer proof.
- Native/integration suites: encodings, Unicode boundaries, tables, search, limits/cancellation, images, clipboard, paths, and isolated registration.
- UI harness: 57 checks in the fullscreen-close run, including Help/About, search, themes, resize, floating zoom controls, table navigation, and toolbar stability.
- Package verifier: static runtime, system-only DLL imports, metadata, size, and ten icon frames.
- Isolated installer identity: install, upgrade, relocation, removal, binary identity, notices, and retained documents/preferences.

See [Building](docs/BUILDING.md) for commands. Tests and review reduce risk; they do not prove absence of bugs. Outstanding platform/environment checks are listed once in [Compatibility](docs/COMPATIBILITY.md#platform); stable-release gates are in [Releasing](docs/RELEASING.md).
