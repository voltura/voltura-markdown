# Staged renderer trial

Software-first startup is the default in 1.1.0 for all document sizes. The measurements below describe validation before release.

## Try it

Build with `scripts/build.ps1`, then launch:

```powershell
.\build\Release\vmd-test.exe "C:\path\document.md" --staged-renderer
```

These diagnostic switches are available only in `vmd-test.exe`; production `vmd.exe` always uses software-first startup. Use `--hardware-renderer` only for diagnostic comparisons with hardware-first startup.

The first page uses software rendering. After that frame succeeds, a private background factory prepares the hardware target. The UI adopts it at the current window size and DPI. The document is not read or parsed again. Text layouts, selection and scroll state remain; image bitmaps are recreated for the new target.

The previous software target stays available until the first hardware frame succeeds. Preparation or first-frame failure leaves the software path available. Closing during preparation keeps the window handle alive until the worker finishes.

## Warm startup measurements

October 17, 2026. Intel Core Ultra 9 185H, Windows 11, 3840 × 2160 at 225%, 60 Hz. Seven runs per mode and file, alternating order. Process-to-content is measured externally by polling the first-content metric file every 5 ms.

| Document | Hardware median | Staged median | Difference |
| --- | ---: | ---: | ---: |
| 1 KiB | 173.0 ms | 106.0 ms | 67.0 ms faster |
| 10 MiB | 427.1 ms | 432.9 ms | 5.9 ms slower |

The small-file ranges overlapped: hardware 157–211 ms, staged 96–221 ms. These are warm local observations, not a guaranteed saving or cold-start result.

Background hardware preparation took a median 75 ms. UI adoption through the first hardware paint took about 8 ms. The hardware frame appeared about 180 ms after app entry for the small file and 508 ms for the 10 MiB file.

Raw results: `build/renderer-ab-1792202151003682700/results.json`.

## Verification

- Native parser/integration tests: 2/2 passed; CommonMark: 652/652 passed.
- Existing UI harness passed in both default and staged modes, including simulated 100–300% DPI changes, fullscreen restoration, selection, zoom and Help/About.
- Closing while hardware preparation was running passed without a hang or crash.
- Resize/DPI change during preparation passed; the promoted target matched the current viewport and DPI.
- Staged 10 MiB forced animated scrolling: 727 paint samples, 0.658 ms mean / 0.931 ms p95 CPU paint time.

The user reported no visible flicker during manual review. That observation is separate from automated checks. Display-level tracing of the renderer transition remains unverified. First-launch-after-reboot results are recorded below.

## Failure recovery

`scripts/test-renderer-failures.py` injects preparation failure and a first hardware-frame device-loss result. Each case must record the failure, then a successful software-rendered document frame. The full UI interaction harness and forced animated scrolling run after each failure.

All four cases passed, including search, selection, zoom, themes, fullscreen, simulated DPI changes, scrolling and closing. The first-frame test substitutes `D2DERR_RECREATE_TARGET` after `EndDraw`; it validates the recovery branch without resetting the physical graphics device. Real driver resets and display-level frame pacing remain unverified.

## Repeat the checks

```powershell
python scripts/benchmark-renderer.py build/bench/1024.md build/bench/10485760.md --runs 7
python scripts/test-renderer-transition.py
python scripts/test-renderer-failures.py
.\scripts\test-display.ps1 -StagedRenderer
.\scripts\test-display.ps1 -StagedRenderer -CloseFullscreen
.\scripts\test-display.ps1 -StagedRenderer -Scroll -Animate -Document build/bench/10485760.md
```

The benchmark expects existing documents. Its JSONL records distinguish first content, background preparation and the first successful hardware paint. The trial scripts write only under `build/` and do not change associations or Windows display settings.

## First launches after separate restarts

Maintainer-run measurements on 2026-10-17: 1 KiB first content in 183 ms from process launch (131 ms from app entry); 10 MiB in 632 ms (580 ms from app entry). Both met the 500 ms / 2 s targets. Both hardware handoffs succeeded with matching viewport and DPI. These are single runs with Windows prefetching unchanged. The maintainer reported no visible flicker.
