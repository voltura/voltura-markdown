"""Measure first-match presentation and background counting in a task-owned viewer."""

import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import pathlib
import subprocess
import time
import uuid

from test_desktop import isolate

isolate()

root = pathlib.Path(__file__).resolve().parents[1]
options = argparse.ArgumentParser(description=__doc__)
options.add_argument("--document", type=pathlib.Path, default=root / "tests/showcase.md")
options.add_argument("--query", default="Voltura")
options.add_argument("--capture", action="store_true")
args = options.parse_args()
folder = root / "build" / ("search-test-" + uuid.uuid4().hex)
folder.mkdir()
metrics = folder / "metrics.jsonl"
u = c.WinDLL("user32", use_last_error=True)
u.SetProcessDpiAwarenessContext(c.c_void_p(-4))
u.SendMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.SendMessageW.restype = c.c_ssize_t
u.GetDlgItem.argtypes = [w.HWND, c.c_int]
u.GetDlgItem.restype = w.HWND
callback_type = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
u.EnumWindows.argtypes = [callback_type, w.LPARAM]
u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
u.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.SetWindowPos.argtypes = [w.HWND, w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.UINT]
u.GetDpiForWindow.argtypes = [w.HWND]
u.GetDpiForWindow.restype = w.UINT


def records():
    result = []
    if metrics.exists():
        for line in metrics.read_text(encoding="utf-8").splitlines():
            try:
                result.append(json.loads(line))
            except json.JSONDecodeError:
                pass  # The app may currently be writing the last record.
    return result


def wait_for(predicate):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        if process.poll() is not None:
            raise RuntimeError("Viewer exited before completing search")
        time.sleep(0.005)
    raise RuntimeError("Search verification timed out")


def window_for(pid):
    handles = []

    @callback_type
    def visit(hwnd, unused):
        owner = w.DWORD()
        u.GetWindowThreadProcessId(hwnd, c.byref(owner))
        name = c.create_unicode_buffer(128)
        u.GetClassNameW(hwnd, name, len(name))
        if owner.value == pid and name.value == "VolturaMarkdownWindow":
            handles.append(hwnd)
        return True

    u.EnumWindows(visit, 0)
    return handles[0] if handles else None


process = subprocess.Popen(
    [
        str(root / "build/Release/vmd-test.exe"),
        str(args.document.resolve()),
        "--metrics",
        str(metrics),
        "--no-preferences",
    ]
)
hwnd = None
try:
    hwnd = wait_for(lambda: window_for(process.pid))
    wait_for(lambda: any("first_content_ms" in row for row in records()))
    u.SendMessageW(hwnd, 0x0111, 101, 0)
    edit = u.GetDlgItem(hwnd, 200)
    query = c.create_unicode_buffer(args.query)
    u.SendMessageW(edit, 0x000C, 0, c.cast(query, c.c_void_p).value)
    total = wait_for(lambda: next((r for r in records() if "search_total" in r), None))
    first = next(r for r in records() if "search_match_paint_ms" in r)
    if first["count_started"] or total["search_total"] < 1:
        raise RuntimeError("Count must start after the first highlighted frame")
    u.SendMessageW(edit, 0x0100, 0x0D, 0)
    # A real keypress is translated into WM_CHAR as well as WM_KEYDOWN.
    # Sending only WM_KEYDOWN misses the single-line edit's default error beep.
    u.SendMessageW(edit, 0x0102, 0x0D, 0)
    paints = wait_for(
        lambda: (
            (r if len(r) >= 2 else None)
            if (r := [row for row in records() if "search_match_paint_ms" in row])
            else None
        )
    )
    if len([r for r in records() if "search_total" in r]) != 1:
        raise RuntimeError("Navigation unexpectedly restarted counting")
    if args.capture:
        # Exercise the reported windowed right edge as well as the fullscreen layout.
        for mode in ("fullscreen", "window", "light", "narrow"):
            if mode == "window":
                u.SendMessageW(hwnd, 0x0111, 104, 0)
            elif mode == "light":
                u.SendMessageW(hwnd, 0x0111, 103, 0)
            elif mode == "narrow":
                scale = u.GetDpiForWindow(hwnd) / 96
                u.SetWindowPos(hwnd, None, 0, 0, round(460 * scale), round(500 * scale), 0x0006)
            time.sleep(0.1)
            subprocess.run(
                [
                    "python",
                    str(root / "scripts/capture-window.py"),
                    str(process.pid),
                    str(folder / f"search-{mode}.png"),
                ],
                check=True,
            )
    print(json.dumps({"first_match": first, "count": total, "next_match": paints[-1]}))
    print(f"Results: {folder}")
finally:
    if hwnd:
        u.PostMessageW(hwnd, 0x0010, 0, 0)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
