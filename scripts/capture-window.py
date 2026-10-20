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
u.SendMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
u.IsWindowVisible.argtypes = [w.HWND]
g.DeleteObject.argtypes = [w.HANDLE]
g.DeleteDC.argtypes = [w.HDC]
u.ReleaseDC.argtypes = [w.HWND, w.HDC]
pid = int(sys.argv[1])
handles = []


@c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
def callback(hwnd, _):
    owner = w.DWORD()
    u.GetWindowThreadProcessId(hwnd, c.byref(owner))
    name = c.create_unicode_buffer(256)
    u.GetClassNameW(hwnd, name, 256)
    if owner.value == pid and u.IsWindowVisible(hwnd) and (len(sys.argv) < 4 or name.value == sys.argv[3]):
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


# D2D HWND targets do not participate in the parent's default PrintWindow path.
# The test executable renders its canvas into a process-local DC; a DC owned
# by this Python process cannot be bound by Direct2D in the application.
canvas_capture = None
u.EnumChildWindows.argtypes = [w.HWND, c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM), w.LPARAM]
u.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
u.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
g.BitBlt.argtypes = [w.HDC, c.c_int, c.c_int, c.c_int, c.c_int, w.HDC, c.c_int, c.c_int, w.DWORD]

@c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
def capture_canvas(child, unused):
    global canvas_capture
    name = c.create_unicode_buffer(128)
    u.GetClassNameW(child, name, len(name))
    if name.value != "VolturaMarkdownCanvas" or not u.IsWindowVisible(child):
        return True
    bounds, client = w.RECT(), w.RECT()
    u.GetWindowRect(child, c.byref(bounds))
    u.GetClientRect(child, c.byref(client))
    if len(sys.argv) > 4:
        path = pathlib.Path(sys.argv[4] + ".canvas.bmp")
        path.unlink(missing_ok=True)
        u.SendMessageW(child, 0x8000 + 45, 0, 0)
        canvas_capture = (path, bounds.left - r.left, bounds.top - r.top)
    return True

u.EnumChildWindows(hwnd, capture_canvas, 0)

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
if canvas_capture:
    path, x, y = canvas_capture
    with Image.open(path) as rendered:
        image.paste(rendered, (x, y))
output = pathlib.Path(sys.argv[2])
output.parent.mkdir(parents=True, exist_ok=True)
image.save(output)
g.SelectObject(memory, old)
g.DeleteObject(bitmap)
g.DeleteDC(memory)
u.ReleaseDC(hwnd, dc)
print(output.resolve())
