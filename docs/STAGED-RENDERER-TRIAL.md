# Staged renderer trial

Experimental branch only. Hardware rendering remains the default. This trial is not version 1.1 and is not published.

## Try it

Build with `scripts/build.ps1`, then launch:

```powershell
.\dist\vmd.exe "C:\path\document.md" --staged-renderer
```

Omit `--staged-renderer` for the existing hardware startup path.

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

The scroll result measures CPU painting, not display-level frame pacing. No claim of flicker-free switching is made from these automated checks. Visual review, display tracing of the transition, cold starts, and injected graphics-device failures remain unverified.

## Repeat the checks

```powershell
python scripts/benchmark-renderer.py build/bench/1024.md build/bench/10485760.md --runs 7
python scripts/test-renderer-transition.py
.\scripts\test-display.ps1 -StagedRenderer
.\scripts\test-display.ps1 -StagedRenderer -CloseFullscreen
.\scripts\test-display.ps1 -StagedRenderer -Scroll -Animate -Document build/bench/10485760.md
```

The benchmark expects existing documents. Its JSONL records distinguish first content, background preparation and first hardware presentation. The trial scripts write only under `build/` and do not change associations or Windows display settings.
