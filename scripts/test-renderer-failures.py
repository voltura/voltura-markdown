"""Inject staged-renderer failures and verify software rendering plus viewer interactions."""

import json
import pathlib
import subprocess
import time

root = pathlib.Path(__file__).resolve().parents[1]
folder = root / "build" / ("renderer-failures-" + str(time.time_ns()))
folder.mkdir(parents=True)
scroll_document = folder / "scroll.md"
scroll_document.write_text(
    (
        "# Fallback scrolling\n\nA paragraph with **bold**, *emphasis*, and Unicode: åäö.\n\n"
        "- First item\n- Second item\n\n"
    )
    * 10000,
    encoding="utf-8",
)
summary = []
for failure in ("preparation", "first-frame"):
    for mode in ("ui", "scroll"):
        name = failure + "-" + mode
        metrics = folder / (name + "-metrics.jsonl")
        checks = folder / (name + "-checks.jsonl")
        document = root / "tests/showcase.md" if mode == "ui" else scroll_document
        command = [
            str(root / "build/Release/vmd.exe"),
            str(document),
            "--staged-renderer",
            "--test-renderer-fail-" + failure,
            "--metrics",
            str(metrics),
            "--no-preferences",
            "--exit-after",
            "35000",
            "--verify-ui" if mode == "ui" else "--scroll-benchmark",
            str(checks),
        ]
        if mode == "scroll":
            command.append("--benchmark-animations")
        process = subprocess.Popen(command)
        try:
            process.wait(timeout=40)
            if process.returncode:
                raise RuntimeError(f"{name}: viewer exit code {process.returncode}")
            frames = [json.loads(line) for line in metrics.read_text().splitlines()]
            checks_rows = [json.loads(line) for line in checks.read_text().splitlines()]
            failures = [row for row in frames if row.get("renderer_promotion") is False]
            if len(failures) != 1 or failures[0]["failure_injection"] != failure.replace("-", "_"):
                raise RuntimeError(f"{name}: expected injected failure missing: {frames}")
            if any(row.get("renderer_promotion") for row in frames):
                raise RuntimeError(f"{name}: unexpected successful hardware promotion")
            failure_index = next(i for i, row in enumerate(frames) if row.get("renderer_promotion") is False)
            if not any(row.get("software_fallback_frame") for row in frames[failure_index + 1 :]):
                raise RuntimeError(f"{name}: no successful software frame after failure")
            if not checks_rows or any(row.get("passed") is False for row in checks_rows):
                raise RuntimeError(f"{name}: UI or scroll checks failed: {checks_rows}")
            if mode == "ui":
                if checks_rows[-1].get("check") != "escape_closes_normal_window":
                    raise RuntimeError(f"{name}: UI harness did not complete")
            elif (
                checks_rows[-1].get("scroll_input_samples", 0) < 1
                or checks_rows[-1].get("scroll_paint_samples", 0) < 1
            ):
                raise RuntimeError(f"{name}: scroll harness did not complete")
            result = {"failure": failure, "mode": mode, "passed": True, "last_check": checks_rows[-1]}
            summary.append(result)
            print(json.dumps(result), flush=True)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
(folder / "results.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
print(folder)
