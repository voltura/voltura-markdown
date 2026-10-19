"""Exercise real manual update shutdown/restart on an isolated desktop and copied binaries."""

import ctypes as c
from ctypes import wintypes as w
import pathlib
import os
import shutil
import subprocess
import time
import uuid
import winreg

from test_desktop import isolate

isolate()
root = pathlib.Path(__file__).resolve().parents[1]
work = root / "build" / f"manual update å {uuid.uuid4().hex}"
work.mkdir()
target = work / "vmd.exe"
helper = work / "helper.exe"
staged = work / "vmd.update-new.exe"
original = root / "build/Release/vmd-test.exe"
for file in (target, helper, staged):
    shutil.copyfile(original, file)
u = c.WinDLL("user32", use_last_error=True)
k = c.WinDLL("kernel32", use_last_error=True)
visit_type = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
u.EnumWindows.argtypes = [visit_type, w.LPARAM]
u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
u.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k.OpenProcess.restype = w.HANDLE
k.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)]
k.CloseHandle.argtypes = [w.HANDLE]


def windows():
    result = []

    @visit_type
    def visit(hwnd, unused):
        name = c.create_unicode_buffer(128)
        u.GetClassNameW(hwnd, name, len(name))
        if name.value != "VolturaMarkdownWindow":
            return True
        pid = w.DWORD()
        u.GetWindowThreadProcessId(hwnd, c.byref(pid))
        process = k.OpenProcess(0x1000, False, pid.value)
        if process:
            path = c.create_unicode_buffer(32768)
            length = w.DWORD(len(path))
            if k.QueryFullProcessImageNameW(process, 0, path, c.byref(length)):
                if pathlib.Path(path.value) == target:
                    result.append((hwnd, pid.value))
            k.CloseHandle(process)
        return True

    u.EnumWindows(visit, 0)
    return result


def wait_for(predicate, message, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.05)
    raise AssertionError(message)


processes = []
try:
    for index in range(3):
        document = work / f"document {index} å.md"
        document.write_text("\n\n".join(f"Paragraph {n}: text " * 10 for n in range(600)), encoding="utf-8")
        report = work / f"view{index}"
        args = [target, "--test-restart-report", report]
        if index < 2:
            args.append(document.name)
        process = subprocess.Popen(args, cwd=work)
        processes.append(process)
        hwnd = wait_for(
            lambda: next((hwnd for hwnd, pid in windows() if pid == process.pid), None),
            "Reader window missing",
        )
        time.sleep(1)
        if index == 0:
            # Keep one window fullscreen, restore the other to a normal window.
            u.PostMessageW(hwnd, 0x100, 0x7A, 0)  # F11
        elif index == 1:
            u.PostMessageW(hwnd, 0x111, 102, 0)  # Source view
            u.PostMessageW(hwnd, 0x20A, (120 << 16) | 8, 0)  # Ctrl+wheel zoom
            time.sleep(0.2)
        u.PostMessageW(hwnd, 0x100, 0x23, 0)  # End
        time.sleep(0.4)
        u.PostMessageW(hwnd, 0x100, 0x21, 0)  # Page Up
        time.sleep(0.5)
    update = subprocess.run([helper, "--manual-update", target, staged], timeout=60)
    assert update.returncode == 0, f"Manual helper failed: {update.returncode}"
    for index in range(3):
        before = work / f"view{index}.before"
        after = work / f"view{index}.after"
        wait_for(lambda: after.exists(), f"Reader {index} did not restore")
        old = [float(value) for value in before.read_text().split()]
        new = [float(value) for value in after.read_text().split()]
        assert old[:8] == new[:8], (old, new)
        if index < 2:
            assert new[8] == 1, "Unchanged document was not recognized"
            assert old[0] > 0, "Fixture did not scroll"
    assert all(process.poll() is not None for process in processes), "Original readers stayed open"
    wait_for(lambda: len(windows()) == 3, f"Unexpected restored window count: {windows()}")
    print(
        f"Manual update: two documents and an empty window restored with exact block/offset, source, zoom, and window state. {work}"
    )
    compiler = shutil.which("makensis") or str(
        pathlib.Path(os.environ["ProgramFiles(x86)"]) / "NSIS/makensis.exe"
    )
    identity = uuid.uuid4().hex
    product_key = f"VolturaMarkdown-Test-{identity}"
    setup = work / "VolturaMarkdown-Test.exe"
    subprocess.run(
        [
            compiler,
            "/WX",
            "/V2",
            "/DVERSION=1.3.0",
            f"/DOUTPUT={setup}",
            f"/DPRODUCT_KEY={product_key}",
            f"/DSHORTCUT_NAME=Voltura Markdown Test {identity}",
            f"/DTEST_INSTALL_DIRECTORY={work}",
            f"/DAPP_BINARY={original}",
            root / "scripts/installer.nsi",
        ],
        check=True,
        timeout=60,
    )
    for index in range(3):
        (work / f"view{index}.after").unlink()
    # NSIS requires /D to be the final, unquoted argument, including spaces.
    subprocess.run(f'"{setup}" /S /D={work}', check=True, timeout=60)
    with winreg.OpenKey(
        winreg.HKEY_CURRENT_USER,
        rf"Software\Microsoft\Windows\CurrentVersion\Uninstall\{product_key}",
        access=winreg.KEY_READ | winreg.KEY_WOW64_64KEY,
    ) as registration:
        assert pathlib.Path(winreg.QueryValueEx(registration, "InstallLocation")[0]) == work
    for index in range(3):
        after = work / f"view{index}.after"
        wait_for(lambda: after.exists(), f"Installer did not restore reader {index}")
        before = [float(value) for value in (work / f"view{index}.before").read_text().split()]
        assert before[:8] == [float(value) for value in after.read_text().split()][:8]
    print("NSIS manual update restored all readers without extra installer UI.")
    # An unrelated lock cannot make the helper terminate its owner. Readers reopen on failure.
    for index in range(3):
        (work / f"view{index}.after").unlink()
    shutil.copyfile(original, staged)
    k.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
    k.CreateFileW.restype = w.HANDLE
    locked = k.CreateFileW(str(target), 0x80000000, 1, None, 3, 0, None)
    assert locked != w.HANDLE(-1).value
    try:
        failed = subprocess.run([helper, "--manual-update", target, staged], timeout=60)
        assert failed.returncode != 0, "Locked replacement succeeded unexpectedly"
    finally:
        k.CloseHandle(locked)
    assert target.read_bytes() == original.read_bytes(), "Failed replacement changed the reader"
    for index in range(3):
        wait_for(lambda: (work / f"view{index}.after").exists(), "Failure did not restore reader")
    print("Replacement failure preserved the executable and restored readers.")
    # Content identity comes from the loaded document, so changes made while it was open are detected.
    (work / "document 0 å.md").write_text("# Changed", encoding="utf-8")
    (work / "document 1 å.md").unlink()
    for index in range(3):
        (work / f"view{index}.after").unlink()
    subprocess.run([helper, "--manual-update", target, staged], check=True, timeout=60)
    for index in range(2):
        after = work / f"view{index}.after"
        wait_for(lambda: after.exists(), "Changed/missing document did not return")
        values = [float(value) for value in after.read_text().split()]
        assert values[0] == values[1] == values[8] == 0, values
    print("Changed and removed documents did not reuse stale scroll positions.")
finally:
    for hwnd, pid in windows():
        u.PostMessageW(hwnd, 0x10, 0, 0)
    deadline = time.monotonic() + 10
    while windows() and time.monotonic() < deadline:
        time.sleep(0.05)
    for hwnd, pid in windows():
        process = k.OpenProcess(1, False, pid)
        if process:
            k.TerminateProcess.argtypes = [w.HANDLE, w.UINT]
            k.TerminateProcess(process, 1)
            k.CloseHandle(process)
    uninstaller = work / "Uninstall.exe"
    if uninstaller.exists():
        subprocess.run(f'"{uninstaller}" /S _?={work}', check=True, timeout=30)
        try:
            winreg.OpenKey(
                winreg.HKEY_CURRENT_USER,
                rf"Software\Microsoft\Windows\CurrentVersion\Uninstall\{product_key}",
                access=winreg.KEY_READ | winreg.KEY_WOW64_64KEY,
            )
        except FileNotFoundError:
            pass
        else:
            raise AssertionError("Test installation registration remained")
