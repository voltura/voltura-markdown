"""Validate the shipped PE, metadata, system-only imports, and embedded icon sizes."""

import ctypes as c
from ctypes import wintypes as w
import json
import os
import pathlib
import struct
import sys

root = pathlib.Path(__file__).resolve().parents[1]
exe = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "dist/vmd.exe"
data = exe.read_bytes()
test_markers = (
    "--verify-ui",
    "--print-pdf",
    "--list-recovery",
    "--metrics",
    "--benchmark",
    "--scroll-benchmark",
    "--no-preferences",
    "--exit-after",
    "--test-association",
    "--test-automatic-updates",
    "--test-update-fixture",
    "--test-restart-report",
    "--test-restart-desktop",
    "download-started",
    "VMD_UPDATES_TEST_PRODUCT_KEY",
    "VMD_UPDATES_TEST_PROFILE",
    "--test-renderer-fail-preparation",
    "--test-renderer-fail-brush",
    "--test-renderer-fail-first-frame",
    "--hardware-renderer",
    "--staged-renderer",
    "--benchmark-animations",
    "--verify-close-fullscreen",
    "search_match_paint_ms",
    "edit_preview_painted",
    "edit_saved_path_matches",
    "new_draft_finish_resets_reader_selection",
    "editor_typing_",
    "editor_capture_",
    "first_content_ms",
    "failure_injection",
    "brush_creation_failure",
    "formatted_default",
    "copied_executable",
    "search_match_keeps_two_rows_of_context_at_each_zoom",
)
for marker in test_markers:
    assert all(
        marker.encode(encoding) not in data for encoding in ("utf-8", "utf-16-le")
    ), f"Test instrumentation in production executable: {marker}"
pe = struct.unpack_from("<I", data, 0x3C)[0]
assert data[pe : pe + 4] == b"PE\0\0", "Not a Windows executable"
_, sections, _, _, _, optional_size, _ = struct.unpack_from("<HHIIIHH", data, pe + 4)
optional = pe + 24
assert struct.unpack_from("<H", data, optional)[0] == 0x20B, "Expected x64 PE"
section_headers = optional + optional_size


def offset(rva):
    for i in range(sections):
        _, virtual_size, address, raw_size, raw_offset = struct.unpack_from(
            "<8sIIII", data, section_headers + i * 40
        )
        if address <= rva < address + max(virtual_size, raw_size):
            return raw_offset + rva - address
    raise AssertionError(f"RVA {rva:x} outside sections")


imports = []
at = offset(struct.unpack_from("<I", data, optional + 112 + 8)[0])
while any(struct.unpack_from("<IIIII", data, at)):
    name = offset(struct.unpack_from("<I", data, at + 12)[0])
    end = data.index(0, name)
    imports.append(data[name:end].decode("ascii"))
    at += 20
for name in imports:
    assert (pathlib.Path(os.environ["WINDIR"]) / "System32" / name).is_file(), f"Non-system DLL: {name}"
assert not any("vcruntime" in s.lower() or "msvcp" in s.lower() for s in imports), "VC runtime must be static"
version = c.WinDLL("version")
version.GetFileVersionInfoSizeW.argtypes = [w.LPCWSTR, c.c_void_p]
version.GetFileVersionInfoW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p]
version.VerQueryValueW.argtypes = [c.c_void_p, w.LPCWSTR, c.POINTER(c.c_void_p), c.POINTER(w.UINT)]
size = version.GetFileVersionInfoSizeW(str(exe), None)
buffer = c.create_string_buffer(size)
assert version.GetFileVersionInfoW(str(exe), 0, size, buffer)
expected = {
    "Author": "Joakim Skoglund",
    "CompanyName": "Voltura AB",
    "ProductName": "Voltura Markdown",
    "FileDescription": "Voltura Markdown",
    "OriginalFilename": "vmd.exe",
    "FileVersion": "1.2.0",
    "ProductVersion": "1.2.0",
}
for key, value in expected.items():
    pointer = c.c_void_p()
    length = w.UINT()
    assert version.VerQueryValueW(
        buffer, "\\StringFileInfo\\040904b0\\" + key, c.byref(pointer), c.byref(length)
    ), key
    assert c.wstring_at(pointer) == value, (key, c.wstring_at(pointer))
kernel = c.WinDLL("kernel32")
kernel.LoadLibraryExW.argtypes = [w.LPCWSTR, w.HANDLE, w.DWORD]
kernel.LoadLibraryExW.restype = w.HMODULE
module = kernel.LoadLibraryExW(str(exe), None, 0x22)
assert module
kernel.FindResourceW.argtypes = [w.HMODULE, c.c_void_p, c.c_void_p]
kernel.FindResourceW.restype = w.HANDLE
kernel.LoadResource.argtypes = [w.HMODULE, w.HANDLE]
kernel.LoadResource.restype = w.HANDLE
kernel.LockResource.argtypes = [w.HANDLE]
kernel.LockResource.restype = c.c_void_p
resource = kernel.FindResourceW(module, c.c_void_p(1), c.c_void_p(14))
assert resource
pointer = kernel.LockResource(kernel.LoadResource(module, resource))
header = c.string_at(pointer, 6)
_, kind, count = struct.unpack("<HHH", header)
assert kind == 1
entries = c.string_at(pointer + 6, count * 14)
sizes = []
for i in range(count):
    width, height, _, _, _, bits, _, _ = struct.unpack_from("<BBBBHHIH", entries, i * 14)
    width = width or 256
    height = height or 256
    assert width == height and bits == 32
    sizes.append(width)
assert sorted(sizes) == [16, 20, 24, 32, 40, 48, 64, 96, 128, 256], sizes
kernel.SizeofResource.argtypes = [w.HMODULE, w.HANDLE]
kernel.SizeofResource.restype = w.DWORD
license_resources = {
    2: root / "third_party/md4c/LICENSE.md",
    3: root / "third_party/nanosvg/LICENSE.txt",
    4: root / "LICENSE",
}
for identifier, path in license_resources.items():
    resource = kernel.FindResourceW(module, c.c_void_p(identifier), c.c_void_p(10))
    assert resource, f"Missing embedded license: {path.name}"
    pointer = kernel.LockResource(kernel.LoadResource(module, resource))
    embedded = c.string_at(pointer, kernel.SizeofResource(module, resource))
    assert embedded == path.read_bytes(), f"Embedded license differs: {path}"
kernel.FreeLibrary.argtypes = [w.HMODULE]
kernel.FreeLibrary(module)
assert len(data) < 10 * 1024 * 1024, "Executable exceeds size ceiling"
print(
    json.dumps(
        {
            "file": str(exe),
            "bytes": len(data),
            "metadata": expected,
            "system_imports": imports,
            "icon_sizes": sizes,
            "embedded_licenses": ["MD4C", "NanoSVG", "Voltura Markdown (MIT)"],
        },
        indent=2,
    )
)
