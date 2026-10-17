"""Alternate hardware and experimental staged rendering on the same documents."""

import argparse
import json
import pathlib
import statistics
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("documents", type=pathlib.Path, nargs="+")
parser.add_argument("--runs", type=int, default=7)
args = parser.parse_args()
if args.runs < 1:
    parser.error("runs must be positive")
root = pathlib.Path(__file__).resolve().parents[1]
folder = root / "build" / ("renderer-ab-" + str(time.time_ns()))
folder.mkdir(parents=True)
results = []
for document in args.documents:
    document = document.resolve(strict=True)
    for iteration in range(args.runs):
        for staged in ([False, True] if iteration % 2 == 0 else [True, False]):
            metric = folder / f"{len(results)}.jsonl"
            command = [
                str(root / "build/Release/vmd.exe"),
                str(document),
                "--benchmark",
                "--no-preferences",
                "--metrics",
                str(metric),
                "--exit-after",
                "10000",
            ]
            command.append("--staged-renderer" if staged else "--hardware-renderer")
            began = time.perf_counter()
            process = subprocess.Popen(command)
            try:
                while not metric.exists():
                    if process.poll() is not None or time.perf_counter() - began > 15:
                        raise RuntimeError("No first-content metric")
                    time.sleep(0.005)
                elapsed = (time.perf_counter() - began) * 1000
                process.wait(timeout=15)
                if process.returncode:
                    raise RuntimeError(f"Viewer failed: {process.returncode}")
                samples = [json.loads(line) for line in metric.read_text().splitlines()]
                if staged and not any(row.get("renderer_promotion") for row in samples):
                    raise RuntimeError(f"Hardware promotion failed: {samples}")
                results.append(
                    {
                        "document": str(document),
                        "staged": staged,
                        "process_to_metric_ms": elapsed,
                        "samples": samples,
                    }
                )
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
    for staged in (False, True):
        group = [row for row in results if row["document"] == str(document) and row["staged"] == staged]
        print(
            json.dumps(
                {
                    "document": str(document),
                    "staged": staged,
                    "median_process_ms": statistics.median(row["process_to_metric_ms"] for row in group),
                    "median_first_content_ms": statistics.median(
                        row["samples"][0]["first_content_ms"] for row in group
                    ),
                }
            ),
            flush=True,
        )
(folder / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
print(folder)
