# Performance and verification

Local x64 MSVC Release measurements, 2026-10-17. Intel Core Ultra 9 185H, 22 logical processors, 63.5 GiB usable RAM; Windows 11 Pro build 26300, 3840×2160 at 225% scaling.

## Optional editing (unreleased, 2026-10-18)

The Windows Rich Edit library loads only when entering edit mode. Typing marks a revision without copying the document; a 400 ms pause captures a snapshot for cancellable background parsing and validation. Known native edits capture only the changed interval; undo, overtype, and uncertain changes use the full-capture fallback. Optional live preview uses the existing renderer and preserves typing focus. Repeated local target checks are cached within each validation pass, and the warning list is capped at 500 entries plus an omitted count. Source highlighting is bounded to visible text. Recovery, image copying, replacement, and export use background workers; these features add no reader-startup work.

Three warm launches per fixture on a private desktop gave median first-content measurements of **68.6 / 75.9 / 456.7 ms** for 1 KiB / 1 MiB / 10 MiB. Median process-to-metric observations were **93.9 / 99.3 / 479.5 ms**. These are current instrumented Release measurements, not a paired before/after comparison or cold-start evidence. Raw samples: `build/bench/edit-mode-final.json`.

After adding the editor tools, three further warm launches measured **69.4 / 78.8 / 465.3 ms** first content and **94.4 / 99.5 / 485.6 ms** process-to-metric for the same sizes (`build/bench/editor-gaps-final.json`). These remain within the startup budgets; the samples are not a controlled before/after comparison. Native synthetic typing measured **1.50 / 1.44 ms** for 1 MiB / 10 MiB, with subsequent changed-region capture at **0.16 / 1.36 ms** (`build/editor-gaps-native.log`). The earlier full-buffer 10 MiB capture cost approximately 72 ms. Capture timings measure source snapshot work, not parser completion or displayed input latency.

The expanded native checks cover Unicode incremental capture, overtype including Tab, undo/redo preservation, bounded source highlighting, outline/fence context, smart list continuation, table operations, recovery cleanup, cancellation, image assets, link rebasing, and HTML export. Private-desktop GUI checks verify continuous-typing crash checkpoints and normal-discard cleanup. Actual Windows PDF output contains five pages and an embedded image; no physical printer was tested. Startup, restart restoration, and simulated display checks also passed after the additions. The unsigned Release executable is 1,247,232 bytes (about 1.19 MiB); the sub-1 MiB package observation below predates editing.

Native single-keystroke samples in 1 MiB and 10 MiB drafts measured **1.57 / 1.31 ms**, including change notification but deferring the full text snapshot. This is a local synthetic input check, not a displayed-keystroke latency measurement. Results: `build/edit-mode-native.log`. A 10 MiB reader search painted the first match in **2.57 ms** and counted 72,818 matches in **68.1 ms**.

Eight CTest suites, the 652 CommonMark cases, production/test separation, formatting, normal/fullscreen reader UI checks, and native editor flows passed. Editing checks cover source helpers and placeholders, Unicode warnings, original encodings/BOMs, safe-save failures/conflicts, Save As, live/toggle preview, undo/redo, scroll retention, keyboard translation/AltGr, help navigation, missing startup files, and restart vetoes. Live preview checks verify successful formatted heading paints, unchanged source focus, side-by-side geometry, stacked geometry, and toolbar overflow. Private-desktop captures inspect native control themes; they do not establish physical-monitor presentation timing or reliable Direct2D screenshot contents.

## Automatic updates (unreleased, 2026-10-18)

Optional cryptographic libraries load on demand. Update checks start five seconds after a successful content/empty-window paint; network, cache, and verification work runs below normal priority and is never joined by the reader on close. Rendering and document parsing are unchanged.

Alternating signed instrumented Release builds on a private Windows desktop, after warm-ups, gave these median process-to-first-content-record times over 12 launches per mode/document: baseline **112.5 / 536.4 ms** (1 KiB / 10 MiB), updates off **113.0 / 543.1 ms**, and updates on **111.0 / 541.0 ms**. Three showcase scroll runs per mode gave median p95 CPU paint times of **5.064 ms** baseline, **5.031 ms** off, **5.094 ms** on, and **5.166 ms** during an active signed-fixture transfer. Median boundary reversals were 123–125. Native traces confirmed the five-second delay; the transfer overlapped scrolling and was cancelled at close. Samples are under `build/update-performance-1ffd1f1733704504801fb662ec1a6e18/`.

These local warm-cache measurements show comparable loading and scrolling. They do not establish identical timings, cold-start performance, or physical displayed-frame pacing. The active transfer used a signed 8 MiB local fixture through the test-only transport; the production WinHTTP client separately fetched the live stable feed and release checksums successfully.

An earlier smaller repeat after cache-error and helper-wait refinements (four launches per mode/document, one scroll run per mode) measured **114.7 / 538.7 ms** baseline, **107.6 / 556.1 ms** off, and **125.3 / 558.3 ms** on. Scroll p95 was **5.131 / 4.976 / 5.137 / 5.074 ms** for baseline/off/on/downloading. Startup differences varied between the runs; all measured medians remained within the existing 500 ms small-file and 2 s 10 MiB budgets. Raw records: `build/update-performance-e9273d61301e452ea34dff1847e7e713/`.

After adding manual update restoration, six alternating launches per mode/document measured **109.0 / 532.6 ms** baseline, **108.4 / 535.1 ms** off, and **108.6 / 537.7 ms** on. The short showcase sometimes fitted entirely after final layout and triggered the benchmark's `scrollable_content_required` guard, so the scroll fixture now repeats it four times. On that longer fixture, two runs per mode initially showed p95 **10.099 / 10.448 / 10.442 / 10.314 ms** (baseline/off/on/downloading). A three-run repeat gave **10.025 / 9.890 / 10.028 / 9.918 ms**, with 14 boundary reversals in every mode; the initial small CPU paint increase did not repeat. Reader rendering has no updater polling or hashing in the scroll path. Records: `build/update-performance-da5b7859ca3149058ffb9019a65f2689/` and `build/update-performance-322a2d56bdd8455c9933a4959a28689a/`. These CPU measurements remain distinct from physical displayed-frame pacing.

Release/CTest/package checks, CommonMark 652/652, native startup/search/display checks, installer lifecycle checks, and isolated signed updater tests passed. Updater coverage includes staged-file corruption, newer/incomplete releases, failed marker writes, publisher pinning, cancellation without joining, real portable replacement, silent NSIS updating, multiple readers, reopening, and late shutdown cancellation. Shutdown/sign-out coverage sends simulated Windows messages; no real shutdown was performed. Test switches, fixtures, and instrumentation remain excluded from production `vmd.exe`.

Manual update checks use real Restart Manager and an isolated NSIS package. Two documents plus an empty window restore exact top-block offsets, source mode, zoom, and fullscreen/window geometry. Replacement failure reopens the original readers; changed and removed content does not reuse stale positions. Snapshot tests cover malformed state, Unicode paths, single use, and a simulated cancelled sign-out. A pre-feature reader also closed successfully during replacement, but cannot supply a saved reading position retroactively.

## 1.1.8 zoom shortcut checks

The shortcuts reuse the existing zoom path only on character input. No startup work, background polling, or rendering changes were added. Native input checks cover translated main-keyboard symbols, numeric keypad input, Ctrl shortcuts applying exactly one step, zoom limits/repetition, control focus, source/Help views, and normal Find text entry. Native Help captures at two window widths show boxed + and - keys without clipping.

Alternating baseline/changed instrumented Release builds, with preferences disabled and identical documents, produced these local warm-cache medians (five launches per build/document): first-content time 127.0 → 123.8 ms for 1 KiB and 483.8 → 477.9 ms for 10 MiB. Three scroll runs per build on the showcase document gave median mean paint time 4.28 → 4.47 ms and median p95 5.00 → 5.20 ms, with 123 median boundary reversals in both builds. These small samples show comparable timings with a small observed scroll difference; they do not establish identical performance or cold-start behavior. Raw samples and captures remain under ignored `build/zoom-ab/`.

## 1.1.7 association notice checks

The existing notice title, explanation, dimensions, and delayed background association probe are retained. Copy path reads the executable location and writes clipboard text only on explicit activation; feedback stays inside the button. Association tests cover Unicode/spaces in a renamed executable path, success/failure feedback, tab order, dismissal, synchronous focus preservation, and unchanged registration/defaults. A test-only clipboard sink leaves the user's clipboard untouched; it validates the requested text and UI behavior, not an actual system clipboard write. Dark/light notice captures were inspected.

The controlled 1.1.6/1.1.7 comparison found no consistent startup or document-loading regression from Copy path. It supersedes the earlier sequential five-launch samples for assessing the version difference; their apparent 21–27 ms startup increase did not reproduce under paired testing.

Both exact release tags were compiled as instrumented x64 Release test builds with the same MSVC/CMake toolchain. Both used the default staged renderer, disabled preferences, and identical document bytes and paths. After two excluded warm-up launches per version/document, each document received 24 adjacent launch pairs with balanced AB/BA version order and shuffled document order: 192 measured launches in total. Only one viewer ran at a time, with 100 ms between process exit and the next launch.

| Document | 1.1.6 median startup | 1.1.7 median startup | Paired mean difference, 1.1.7 minus 1.1.6 (95% CI) |
| --- | ---: | ---: | ---: |
| 1 KiB | 124.1 ms | 123.2 ms | +2.2 ms (−5.4 to +10.1) |
| 1 MiB | 126.4 ms | 123.9 ms | −3.5 ms (−11.3 to +5.1) |
| 10 MiB | 367.4 ms | 345.4 ms | −7.5 ms (−23.2 to +6.4) |
| Voltura Air `docs/protocol.md` | 121.7 ms | 127.2 ms | +1.7 ms (−6.3 to +9.1) |

Startup measures process creation to a complete first-content metric record, polled at 1 ms. The difference column is the mean of within-pair differences, not the difference between the two medians. Confidence intervals use 10,000 paired bootstrap resamples; no measured samples were removed. All startup intervals include zero. Separately measured file reading, parsing, and document-open-to-first-painted-content timings also had paired mean-difference intervals containing zero.

These results show no statistically distinguishable regression in this local run, not proof of identical performance. Small differences remain possible within the intervals. This was an ordinary warm-cache test without a reboot or cache flush, using instrumented test executables rather than the signed production downloads; it does not establish cold-start behavior or results on other machines. Detailed results, raw samples, executable/fixture hashes, and source commit IDs are retained under the ignored `build/compare-1.1.6-1.1.7/paired-run/` directory (`REPORT.md`, `summary.json`, `samples.jsonl`, and `manifest.json`).

For historical context, the earlier sequential five-launch medians were 82 → 103 ms for 1 KiB and 312 → 339 ms for 10 MiB (`build/bench/copy-path-before.json`, `build/bench/copy-path-after-idle.json`). Those unpaired samples were taken at different times and do not establish that Copy path caused a slowdown.

The separate 10 MiB search check painted its first match in 3.9 ms, navigation in 2.4 ms, and counted 72,818 matches in 46 ms. Native/package tests, both UI exit paths, 652 CommonMark cases, formatting, and the isolated installer lifecycle passed.

## 1.1.6 layout regression checks

Native geometry checks first reproduced nested-table border/background misalignment, ignored image indentation, unscaled placeholder offsets, and uneven/gapped backgrounds between bounded code sections. The corrected checks pass at 50%, 100%, 200%, and 300% document zoom. Wide-code padding and horizontal scrolling also pass. Actual native-window captures at 300% were inspected for quoted tables, loaded and missing images, and wide code in dark/light themes; these are local screenshots, not a new physical-monitor/DPI certification.

Five launches per fixture with ordinary filesystem caching gave median process-to-first-content times of 104 → 107 ms for 1 KiB and 364 → 372 ms for 10 MiB before/after the additional layout fixes. Peak 10 MiB viewer memory stayed at 203.4 MiB. These small sample differences do not establish a statistically significant change. Results: `build/bench/layout-before.json` and `build/bench/layout-after.json`.

The 10 MiB first search match painted in 3.9 ms; counting 72,818 matches took 46 ms. Separate ten-second hardware-renderer scroll samples measured CPU paint mean/p95 of 0.79/0.84 ms before and 0.83/0.95 ms after. These are CPU measurements, not displayed-frame timing. The implementation retains bounded layouts and measures only the visible viewport before painting; code-group metadata fits existing block padding without enlarging the per-block record.

Both UI exit paths, simulated DPI transitions, native suites, 652 CommonMark examples, package isolation, formatting, and the isolated installer lifecycle passed.

## 1.1.5 regression checks

Three launches per fixture with ordinary filesystem caching, after the reliability fixes:

| Input | Median process launch to first-content metric | Peak viewer working set |
| --- | ---: | ---: |
| 1 KiB | 97 ms | 86.7 MiB |
| 1 MiB | 120 ms | 98.1 MiB |
| 10 MiB | 325 ms | 203.2 MiB |
| 100 MiB stress | 2.57 s | 1,300.6 MiB |

These are local warm/cache-dependent samples, not new reboot or physical-display measurements. Results are retained in `build/bench/release-1.1.5.json`. Search on the 10 MiB fixture presented its first match in 4.2 ms and completed the count of 72,818 matches in 52 ms.

A malformed 800 KB HTML entity fixture improved from roughly 8 seconds to 7 ms of parsing; a 1.6 MB variant completed in 19 ms. Tiny SVG dash fixtures now fail without blocking shutdown. SVG CSS, transform and dash regression checks also passed with MSVC AddressSanitizer. Earlier measurements below retain their original renderer and cache conditions.

## Startup and memory

Three separate launches per generated document, with ordinary Windows filesystem caching:

| Input | Median first content (app entry) | Median process launch to metric | Median read + parse | Peak parser working set | Peak viewer working set |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 KiB | 135 ms | 159 ms | 0.30 ms | 3.6 MiB | 92.1 MiB |
| 1 MiB | 170 ms | 191 ms | 35 ms | 16.3 MiB | 103.3 MiB |
| 10 MiB | 511 ms | 534 ms | 457 ms | 126.8 MiB | 206.7 MiB |
| 100 MiB stress | 3.54 s | 3.56 s | 3.07 s | 1,224.3 MiB | 1,304.0 MiB |

The dense 100 MiB fixture contains 3,640,889 blocks. Before optimization, the same fixture used 2,175.7 MiB peak parser memory and took 4.52 s to first content. The revised model reduces that peak by 44% and first-content time by 22% in these runs. Segmented block storage avoids whole-list growth copies, plain text has implicit formatting, optional metadata allocates on demand, source-view segmentation is lazy, and the initial height index builds in linear time.

The app-entry metric excludes OS loader time. The external metric includes process creation and approximately 5 ms polling granularity. The final benchmark launches the viewer before its parser-only run. Neither method purges Windows caches: these are not verified cold-cache measurements. The 1 KiB process-launch result narrowly misses the 150 ms warm target; the 10 MiB result meets the 1 s warm target. Results vary with system load and workload.

A UI heartbeat sampled loading and first-layout work. The largest observed gap was 44 ms for 10 MiB and 124 ms for the dense 100 MiB stress case. This is message-loop scheduling evidence, not an end-to-end keyboard latency measurement. Full source and parsed structure still scale with document size; the stress case is not a constant-memory reader.

Pathological parsing: a 10 MiB unbroken paragraph took 71 ms; a 10 MiB fenced-code line 53 ms; a table with a 1 MiB cell 12 ms. Long content remains segmented for bounded layout work.

Run `python scripts/benchmark.py --output acceptance-results.json`. Generated fixtures and detailed results stay under ignored `build/bench/`. See [Building](docs/BUILDING.md#display-and-startup-validation) for first-launch and display-trace commands.

## First launch after reboot

The maintainer ran each case after a separate Windows restart on 2026-10-17, before opening the reader. Existing fixtures were generated beforehand.

| Input | App entry to first content | Process launch to metric | Target | Result |
| --- | ---: | ---: | ---: | --- |
| 1 KiB | 178 ms | 232 ms | 500 ms | Pass |
| 10 MiB | 468 ms | 564 ms | 2,000 ms | Pass |

One sample per case, saved as `build/bench/cold-small.json` and `cold-10mb.json`. These validate first launch after reboot; Windows prefetching was not disabled and an empty filesystem cache is not asserted.

## Rendering

The document canvas is separate from the buffered toolbar. Viewport layouts and narrow invalidation limit redraw work; scrolling honors Windows' reduced-motion setting.

A ten-second 10 MiB scrolling test at 60 Hz produced 722 CPU paint samples (0.53 ms mean / 0.86 ms p95). The input driver issued 629 updates; it uses a 10 ms Windows timer because requesting 16 ms under coarse timer scheduling can undersupply a 60 Hz display.

An elevated PresentMon 2.6.0 capture run by the maintainer measured **59.6 displayed updates/s** at 3840×2160, 60 Hz and 225% scaling. Excluding the first 500 ms and last 200 ms, 550 of 554 displayed intervals were approximately 16.7 ms, with four at 33.3 ms. GPU work averaged 1.87 ms. A preceding 30 Hz trace measured 29.8 displayed updates/s. CSV records are filtered to the test-owned process.

Windows interface animations were disabled during the display capture, and the reader honored that setting. A separate 100 MiB test forced smooth scrolling inside the test process without changing Windows preferences: 724 CPU paint samples, 0.53 ms mean / 0.84 ms p95. This second test did not collect display-level timings.

Startup profiling attributes roughly 130 ms to initial hardware render-target setup in the sampled runs. An explicitly software target reduced small-file process-to-content time to 145 ms, but increased scrolling paint cost to 8.8 ms mean / 11.7 ms p95. Hardware rendering is retained for rendering efficiency. A subsequent seven-run warm sample at 60 Hz measured 186 ms median process-to-content for 1 KiB and 402 ms for 10 MiB. The small-file 150 ms target remains a measured miss.

## Validation

- 652/652 CommonMark 0.31.2 examples: normalized parser HTML and adapter bounds, not pixel-identical renderer proof.
- Native/integration suites: encodings, Unicode, tables, search, cancellation, sparse formatting, segmented code metadata, images, clipboard, paths and isolated registration.
- UI harness: windowed/fullscreen simulated DPI transitions through 100%, 125%, 150%, 200%, 225%, 300% and back; negative window coordinates; render-target DPI, control sizes, zoom clearance, preserved selection/theme and height-index consistency. Existing Help/About, source view, search, themes and both close paths also pass.
- DPI simulation exercises Windows message handling without altering display settings. It does not certify physical monitor topology, driver behavior or visual sharpness across real monitors.
- Package checks: static runtime, system-only imports, embedded metadata/licenses and ten icon frames; executable remains below 1 MiB.
- Maintainer reports clean Windows execution and default-app association checks completed. Automated installer lifecycle tests cover install, upgrade, relocation, removal and retained documents/preferences.

First-launch-after-reboot targets pass. The maintainer accepted retaining hardware rendering for 1.0 despite the small-file warm-target miss. Physical mixed-DPI movement is additional unverified hardware coverage; simulated scale transitions pass. Display-frame delivery at 60 Hz is validated for the 10 MiB workload. The small-file warm-start miss is recorded above. See [Compatibility](docs/COMPATIBILITY.md#platform) and [Releasing](docs/RELEASING.md).

## Version 1.1 startup

Software-first rendering is now the default, without file-size routing. Seven alternating warm runs measured 173 ms hardware-first versus 106 ms software-first for 1 KiB; 427 ms versus 433 ms for 10 MiB. Separate first launches after reboot measured 183 ms for 1 KiB and 632 ms for 10 MiB, both within the startup targets. These single reboot runs do not establish a statistically controlled speed comparison. Hardware handoff succeeded in both.

Injected hardware-creation and first-frame failures retained functional software rendering; UI and scrolling checks passed. The maintainer observed no visible flicker. See [startup validation](docs/STAGED-RENDERER-TRIAL.md) for methods and limits.
