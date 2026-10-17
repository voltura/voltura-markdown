"""Check native startup window modes without changing saved preferences."""

import ctypes
import ctypes.wintypes as wt
import pathlib
import subprocess
import time

root = pathlib.Path(__file__).resolve().parents[1]
user32 = ctypes.WinDLL("user32", use_last_error=True)
callback_type = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
user32.EnumWindows.argtypes = [callback_type, wt.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.GetWindowLongW.argtypes = [wt.HWND, ctypes.c_int]
user32.IsWindowVisible.argtypes = [wt.HWND]
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]


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


for with_document in (False, True):
    args = [str(root / "build/Release/vmd-test.exe"), "--no-preferences"]
    if with_document:
        args.append(str(root / "tests/showcase.md"))
    process = subprocess.Popen(args)
    hwnd = None
    try:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            hwnd = window_for(process.pid)
            if hwnd and user32.IsWindowVisible(hwnd):
                break
            if process.poll() is not None:
                raise RuntimeError("Viewer exited before showing its window")
            time.sleep(0.01)
        else:
            raise RuntimeError("Viewer window did not appear")
        caption = bool(user32.GetWindowLongW(hwnd, -16) & 0x00C00000)
        if caption == with_document:
            raise RuntimeError(f"Unexpected startup mode: document={with_document}")
        mode = "document launch is fullscreen" if with_document else "empty launch is windowed"
        print(f"PASS: {mode}")
    finally:
        if hwnd:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
