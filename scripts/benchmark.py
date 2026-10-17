"""Reproducible local parsing + GUI first-content benchmark. Writes only build/."""

import argparse
import json
import pathlib
import subprocess
import statistics
import sys
import time

options = argparse.ArgumentParser(description=__doc__)
options.add_argument("--runs", type=int, default=3)
options.add_argument("--sizes", type=int, nargs="+", default=[1024, 1024**2, 10 * 1024**2, 100 * 1024**2])
options.add_argument("--output", default="results.json")
options.add_argument(
    "--startup-only",
    action="store_true",
    help="Launch GUI before any parser run; skips parser/pathological cases",
)
args = options.parse_args()
if args.runs < 1 or any(size < 1 for size in args.sizes):
    options.error("Runs and document sizes must be positive")
ROOT = pathlib.Path(__file__).resolve().parents[1]
folder = ROOT / "build/bench"
folder.mkdir(parents=True, exist_ok=True)
app = ROOT / "build/Release/vmd.exe"
parser = ROOT / "build/Release/reader_tests.exe"
pattern = b"# Voltura performance\n\nA paragraph with **bold**, *emphasis*, Unicode: \xc3\xa5\xc3\xa4\xc3\xb6. Read, copy and scroll.\n\n- One\n- Two\n\n```cpp\nint value = 42;\n```\n\n"
results = []
for size in args.sizes:
    path = folder / f"{size}.md"
    if not path.exists() or path.stat().st_size != size:
        path.write_bytes((pattern * (size // len(pattern) + 1))[:size])
    parses = []
    gui = []
    for iteration in range(args.runs):
        metric = folder / f"metric-{size}-{iteration}.jsonl"
        if metric.exists():
            metric.unlink()
        launched_at = time.perf_counter()
        process = subprocess.Popen(
            [
                str(app),
                str(path),
                "--metrics",
                str(metric),
                "--benchmark",
                "--exit-after",
                "20000",
                "--no-preferences",
            ]
        )
        try:
            while not metric.exists():
                if process.poll() is not None or time.perf_counter() - launched_at > 25:
                    raise RuntimeError(f"Benchmark exited or timed out before first content for {size}")
                time.sleep(0.005)
            process_to_metric_ms = (time.perf_counter() - launched_at) * 1000
            process.wait(timeout=25)
            if process.returncode:
                raise RuntimeError(f"Benchmark process failed: {process.returncode}")
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        if not metric.exists():
            raise RuntimeError(f"No first content metric for {size}")
        sample = json.loads(metric.read_text().splitlines()[0])
        sample["process_to_metric_ms"] = process_to_metric_ms
        gui.append(sample)
        if not args.startup_only:
            p = subprocess.run([str(parser), "--benchmark", str(path)], capture_output=True, check=True)
            parses.append(json.loads(p.stdout))
    result = {
        "bytes": size,
        "parse_runs": parses,
        "gui_runs": gui,
        "median_parse_ms": statistics.median(x["total_ms"] for x in parses) if parses else None,
        "median_first_content_ms": statistics.median(x["first_content_ms"] for x in gui),
        "peak_working_set_bytes": max((x["peak_working_set_bytes"] for x in parses), default=None),
    }
    results.append(result)
    print(json.dumps(result), flush=True)
specials = {
    "long-paragraph.md": b"a" * (10 * 1024**2),
    "long-code.md": b"```\n" + b"x" * (10 * 1024**2) + b"\n```",
    "long-table.md": b"| A | B |\n| - | - |\n| " + b"x" * (1024**2) + b" | value |\n",
}
for name, data in (specials.items() if not args.startup_only else []):
    path = folder / name
    path.write_bytes(data)
    p = subprocess.run([str(parser), "--benchmark", str(path)], capture_output=True, check=True)
    result = {"case": name, **json.loads(p.stdout)}
    results.append(result)
    print(json.dumps(result), flush=True)
(folder / args.output).write_text(json.dumps(results, indent=2), encoding="utf-8")
