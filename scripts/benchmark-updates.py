"""Compare updater startup/scrolling against a saved baseline; writes only build/.

The active-download case needs a signed test reader and a signed fixture directory
containing release.json, SHA256SUMS.txt, and a sufficiently large payload.exe.
"""

import argparse
import json
import pathlib
import statistics
import subprocess
import time
import uuid

from test_desktop import isolate

isolate()

root = pathlib.Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--baseline", type=pathlib.Path, required=True)
parser.add_argument("--current", type=pathlib.Path, required=True)
parser.add_argument("--fixture", type=pathlib.Path, required=True)
parser.add_argument("--runs", type=int, default=12)
parser.add_argument("--scroll-runs", type=int, default=3)
args = parser.parse_args()
out = root / "build" / ("update-performance-" + uuid.uuid4().hex)
out.mkdir()
pattern = b"# Voltura performance\n\nA paragraph with **bold** and *emphasis*. Read, copy and scroll.\n\n- One\n- Two\n\n```cpp\nint value = 42;\n```\n\n"
modes = {
    "baseline": (args.baseline.resolve(), []),
    "disabled": (args.current.resolve(), []),
    "enabled": (args.current.resolve(), ["--test-automatic-updates"]),
    "downloading": (args.current.resolve(), ["--test-update-fixture", str(args.fixture.resolve())]),
}
samples = []


def run(mode, document, scroll=False):
    exe, extra = modes[mode]
    identity = f"{len(samples)}-{mode}"
    metric = out / (identity + ".jsonl")
    checks = out / (identity + "-checks.jsonl")
    started_marker = args.fixture / "download-started"
    finished_marker = args.fixture / "download-finished"
    if mode == "downloading":
        started_marker.unlink(missing_ok=True)
        finished_marker.unlink(missing_ok=True)
    options = ["--scroll-benchmark", str(checks)] if scroll else ["--benchmark"]
    start = time.perf_counter()
    process = subprocess.Popen(
        [str(exe), str(document), "--metrics", str(metric), "--no-preferences", *options, *extra]
    )
    first_observed = None
    try:
        deadline = start + 40
        while process.poll() is None:
            if first_observed is None and metric.exists():
                try:
                    if any(
                        "first_content_ms" in json.loads(line) for line in metric.read_text().splitlines()
                    ):
                        first_observed = (time.perf_counter() - start) * 1000
                except json.JSONDecodeError:
                    pass
            if time.perf_counter() > deadline:
                raise RuntimeError(f"Timed out: {identity}")
            time.sleep(0.001)
        if process.returncode:
            raise RuntimeError(f"Viewer failed: {identity}")
        records = [json.loads(line) for line in metric.read_text().splitlines()]
        first = next(row for row in records if "first_content_ms" in row)
        row = {
            "mode": mode,
            "document": document.name,
            "scroll": scroll,
            "observed_ms": first_observed,
            "first": first,
        }
        if scroll:
            result = [json.loads(line) for line in checks.read_text().splitlines()]
            if any(r.get("passed") is False for r in result):
                raise RuntimeError(f"Behavior check failed: {checks}")
            row["paint"] = next(r for r in result if "paint_p95_ms" in r)
            if row["paint"]["scroll_boundary_reversals"] < 1:
                raise RuntimeError("Scrolling did not reverse at boundaries")
            if mode in ("enabled", "downloading"):
                timing = next(r["update_check_ms"] for r in records if "update_check_ms" in r)
                row["check_ms"] = timing
                if timing < first["first_content_ms"] + 4900:
                    raise RuntimeError("Update check ran before five-second readiness delay")
            if mode == "downloading":
                if not started_marker.exists() or finished_marker.exists():
                    raise RuntimeError("Expected active transfer during scrolling and cancellation at close")
        samples.append(row)
        (out / "samples.json").write_text(json.dumps(samples, indent=2))
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


documents = []
for size in (1024, 10 * 1024**2):
    path = out / f"{size}.md"
    path.write_bytes((pattern * (size // len(pattern) + 1))[:size])
    documents.append(path)
# Warm both binaries; omit these samples from summaries while retaining raw records.
for mode in ("baseline", "disabled", "enabled"):
    for document in documents:
        run(mode, document)
warmups = len(samples)
scroll_document = out / "scroll-showcase.md"
# The short showcase can fit completely on a tall desktop after final layout.
# Repeat it so every run must exercise actual scrolling and boundary reversals.
scroll_document.write_text((root / "tests/showcase.md").read_text(encoding="utf-8") * 4, encoding="utf-8")
for iteration in range(args.runs):
    order = ("baseline", "disabled", "enabled") if iteration % 2 == 0 else ("enabled", "disabled", "baseline")
    for document in documents:
        for mode in order:
            run(mode, document)
for iteration in range(args.scroll_runs):
    order = list(modes) if iteration % 2 == 0 else list(reversed(modes))
    for mode in order:
        run(mode, scroll_document, True)
summary = {"startup": {}, "scroll": {}, "output": str(out)}
for mode in modes:
    for document in documents:
        rows = [
            r
            for r in samples[warmups:]
            if r["mode"] == mode and not r["scroll"] and r["document"] == document.name
        ]
        if rows:
            summary["startup"][f"{mode}/{document.name}"] = {
                "n": len(rows),
                "median_ms": statistics.median(r["first"]["first_content_ms"] for r in rows),
                "observed_ms": statistics.median(
                    r["observed_ms"] for r in rows if r["observed_ms"] is not None
                ),
            }
    rows = [r for r in samples if r["mode"] == mode and r["scroll"]]
    summary["scroll"][mode] = {
        "n": len(rows),
        "mean_ms": statistics.median(r["paint"]["paint_mean_ms"] for r in rows),
        "p95_ms": statistics.median(r["paint"]["paint_p95_ms"] for r in rows),
        "reversals": statistics.median(r["paint"]["scroll_boundary_reversals"] for r in rows),
    }
(out / "summary.json").write_text(json.dumps(summary, indent=2))
print(json.dumps(summary, indent=2))
