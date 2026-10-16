"""Reproducible local parsing + GUI first-content benchmark. Writes only build/."""

import json
import pathlib
import subprocess
import statistics
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
folder = ROOT / "build/bench"
folder.mkdir(parents=True, exist_ok=True)
app = ROOT / "build/Release/vmd.exe"
parser = ROOT / "build/Release/reader_tests.exe"
pattern = b"# Voltura performance\n\nA paragraph with **bold**, *emphasis*, Unicode: \xc3\xa5\xc3\xa4\xc3\xb6. Read, copy and scroll.\n\n- One\n- Two\n\n```cpp\nint value = 42;\n```\n\n"
results = []
for size in [1024, 1024**2, 10 * 1024**2, 100 * 1024**2]:
    path = folder / f"{size}.md"
    if not path.exists() or path.stat().st_size != size:
        path.write_bytes((pattern * (size // len(pattern) + 1))[:size])
    parses = []
    gui = []
    for iteration in range(3):
        p = subprocess.run([str(parser), "--benchmark", str(path)], capture_output=True, check=True)
        parses.append(json.loads(p.stdout))
        metric = folder / f"metric-{size}-{iteration}.jsonl"
        if metric.exists():
            metric.unlink()
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
            process.wait(timeout=25)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait()
            raise RuntimeError("Task-owned benchmark app did not exit")
        if not metric.exists():
            raise RuntimeError(f"No first content metric for {size}")
        gui.append(json.loads(metric.read_text().splitlines()[0]))
    result = {
        "bytes": size,
        "parse_runs": parses,
        "gui_runs": gui,
        "median_parse_ms": statistics.median(x["total_ms"] for x in parses),
        "median_first_content_ms": statistics.median(x["first_content_ms"] for x in gui),
        "peak_working_set_bytes": max(x["peak_working_set_bytes"] for x in parses),
    }
    results.append(result)
    print(json.dumps(result), flush=True)
specials = {
    "long-paragraph.md": b"a" * (10 * 1024**2),
    "long-code.md": b"```\n" + b"x" * (10 * 1024**2) + b"\n```",
    "long-table.md": b"| A | B |\n| - | - |\n| " + b"x" * (1024**2) + b" | value |\n",
}
for name, data in specials.items():
    path = folder / name
    path.write_bytes(data)
    p = subprocess.run([str(parser), "--benchmark", str(path)], capture_output=True, check=True)
    result = {"case": name, **json.loads(p.stdout)}
    results.append(result)
    print(json.dumps(result), flush=True)
(folder / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
