"""Capture only a task-owned app window by PID, without changing focus."""

import ctypes as c
from ctypes import wintypes as w
import pathlib
import sys
from PIL import Image

u = c.WinDLL("user32", use_last_error=True)
g = c.WinDLL("gdi32", use_last_error=True)
u.SetProcessDpiAwarenessContext(c.c_void_p(-4))
u.GetDC.restype = w.HDC
g.CreateCompatibleDC.restype = w.HDC
g.CreateCompatibleBitmap.restype = w.HBITMAP
g.SelectObject.restype = w.HANDLE
g.CreateCompatibleBitmap.argtypes = [w.HDC, c.c_int, c.c_int]
g.CreateCompatibleDC.argtypes = [w.HDC]
g.SelectObject.argtypes = [w.HDC, w.HANDLE]
u.PrintWindow.argtypes = [w.HWND, w.HDC, w.UINT]
g.DeleteObject.argtypes = [w.HANDLE]
g.DeleteDC.argtypes = [w.HDC]
u.ReleaseDC.argtypes = [w.HWND, w.HDC]
pid = int(sys.argv[1])
handles = []


@c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
def callback(hwnd, _):
    owner = w.DWORD()
    u.GetWindowThreadProcessId(hwnd, c.byref(owner))
    if owner.value == pid and u.IsWindowVisible(hwnd):
        handles.append(hwnd)
    return True


u.EnumWindows(callback, 0)
if not handles:
    raise SystemExit("No task-owned window found")
hwnd = handles[0]
r = w.RECT()
u.GetWindowRect(hwnd, c.byref(r))
width = r.right - r.left
height = r.bottom - r.top
dc = u.GetDC(hwnd)
memory = g.CreateCompatibleDC(dc)
bitmap = g.CreateCompatibleBitmap(dc, width, height)
old = g.SelectObject(memory, bitmap)
if not u.PrintWindow(hwnd, memory, 2):
    raise SystemExit("PrintWindow failed")


class Header(c.Structure):
    _fields_ = [
        ("size", w.DWORD),
        ("width", w.LONG),
        ("height", w.LONG),
        ("planes", w.WORD),
        ("bits", w.WORD),
        ("compression", w.DWORD),
        ("imageSize", w.DWORD),
        ("x", w.LONG),
        ("y", w.LONG),
        ("used", w.DWORD),
        ("important", w.DWORD),
    ]


header = Header(c.sizeof(Header), width, -height, 1, 32, 0, 0, 0, 0, 0, 0)
pixels = c.create_string_buffer(width * height * 4)
g.GetDIBits.argtypes = [w.HDC, w.HBITMAP, w.UINT, w.UINT, c.c_void_p, c.c_void_p, w.UINT]
g.GetDIBits(memory, bitmap, 0, height, pixels, c.byref(header), 0)
image = Image.frombuffer("RGB", (width, height), pixels, "raw", "BGRX", 0, 1)
output = pathlib.Path(sys.argv[2])
output.parent.mkdir(parents=True, exist_ok=True)
image.save(output)
g.SelectObject(memory, old)
g.DeleteObject(bitmap)
g.DeleteDC(memory)
u.ReleaseDC(hwnd, dc)
print(output.resolve())
