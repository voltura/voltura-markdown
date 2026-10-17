# Performance and verification

Local x64 MSVC Release measurements, 2026-10-17. Intel Core Ultra 9 185H, 22 logical processors, 63.5 GiB usable RAM; Windows 11 Pro build 26300, 3840×2160 at 225% scaling.

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
