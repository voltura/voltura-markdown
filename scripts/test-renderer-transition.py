"""Exercise owned viewer windows during experimental renderer preparation."""

import ctypes
import ctypes.wintypes as wt
import json
import pathlib
import subprocess
import time

from test_desktop import isolate

isolate()

root = pathlib.Path(__file__).resolve().parents[1]
folder = root / "build" / ("renderer-transition-" + str(time.time_ns()))
folder.mkdir(parents=True)
document = folder / "sample.md"
document.write_text("# Renderer transition\n\nA **formatted** paragraph.\n", encoding="utf-8")
user32 = ctypes.WinDLL("user32", use_last_error=True)
callback_type = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
user32.EnumWindows.argtypes = [callback_type, wt.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.SendMessageW.restype = wt.LPARAM


def window_for(pid):
    windows = []

    @callback_type
    def visit(hwnd, unused):
        owner = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        name = ctypes.create_unicode_buffer(128)
        user32.GetClassNameW(hwnd, name, len(name))
        if owner.value == pid and name.value == "VolturaMarkdownWindow":
            windows.append(hwnd)
        return True

    user32.EnumWindows(visit, 0)
    return windows[0] if windows else None


for action in ("close", "dpi_resize"):
    metric = folder / (action + ".jsonl")
    process = subprocess.Popen(
        [
            str(root / "build/Release/vmd-test.exe"),
            str(document),
            "--staged-renderer",
            "--no-preferences",
            "--metrics",
            str(metric),
            "--exit-after",
            "1200",
        ]
    )
    try:
        deadline = time.monotonic() + 5
        while not metric.exists() or '"renderer_preparing":true' not in metric.read_text():
            if time.monotonic() > deadline or process.poll() is not None:
                raise RuntimeError("Renderer preparation did not start")
            time.sleep(0.001)
        hwnd = window_for(process.pid)
        if not hwnd:
            raise RuntimeError("Viewer window not found")
        if action == "close":
            user32.PostMessageW(hwnd, 0x10, 0, 0)
        else:
            bounds = wt.RECT()
            user32.GetWindowRect(hwnd, ctypes.byref(bounds))
            bounds.right = bounds.left + 1100
            bounds.bottom = bounds.top + 750
            user32.SendMessageW(hwnd, 0x2E0, 144 | (144 << 16), ctypes.addressof(bounds))
        process.wait(timeout=5)
        if process.returncode:
            raise RuntimeError(f"Viewer failed: {process.returncode}")
        rows = [json.loads(line) for line in metric.read_text().splitlines()]
        if action == "close":
            assert any(row.get("renderer_close_deferred") for row in rows), rows
        else:
            assert any(row.get("renderer_promotion") and row.get("viewport_matches") for row in rows), rows
        print(json.dumps({"check": action + "_during_preparation", "passed": True}))
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
print(folder)
