"""Verify delayed association checks in owned viewer processes; never change real defaults."""

import ctypes as c
from ctypes import wintypes as w
import json
import importlib.util
import pathlib
import shutil
import subprocess
import sys
import time
import winreg

root = pathlib.Path(__file__).resolve().parents[1]
folder = root / "build" / ("association-checks-" + str(time.time_ns()))
folder.mkdir(parents=True)
u = c.WinDLL("user32", use_last_error=True)
callback_type = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
u.EnumWindows.argtypes = [callback_type, w.LPARAM]
u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
u.GetWindowThreadProcessId.restype = w.DWORD
u.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
u.FindWindowExW.argtypes = [w.HWND, w.HWND, w.LPCWSTR, w.LPCWSTR]
u.FindWindowExW.restype = w.HWND
u.GetDlgItem.argtypes = [w.HWND, c.c_int]
u.GetDlgItem.restype = w.HWND
u.IsWindowVisible.argtypes = [w.HWND]
u.IsWindowEnabled.argtypes = [w.HWND]
u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.SendMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.SendMessageW.restype = w.LPARAM
u.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
u.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]


class GuiInfo(c.Structure):
    _fields_ = (
        [("size", w.DWORD), ("flags", w.DWORD)]
        + [(name, w.HWND) for name in ("active", "focus", "capture", "menu", "move", "caret")]
        + [("rect", w.RECT)]
    )


u.GetGUIThreadInfo.argtypes = [w.DWORD, c.POINTER(GuiInfo)]


def focus(hwnd):
    info = GuiInfo()
    info.size = c.sizeof(info)
    thread = u.GetWindowThreadProcessId(hwnd, None)
    assert u.GetGUIThreadInfo(thread, c.byref(info))
    return info.focus


def label(hwnd):
    text = c.create_unicode_buffer(128)
    u.GetWindowTextW(hwnd, text, len(text))
    return text.value


def bounds(hwnd):
    rect = w.RECT()
    assert u.GetWindowRect(hwnd, c.byref(rect))
    return (rect.left, rect.top, rect.right, rect.bottom)


def window_for(pid, class_name="VolturaMarkdownWindow"):
    windows = []

    @callback_type
    def visit(hwnd, unused):
        owner = w.DWORD()
        u.GetWindowThreadProcessId(hwnd, c.byref(owner))
        name = c.create_unicode_buffer(128)
        u.GetClassNameW(hwnd, name, len(name))
        if owner.value == pid and name.value == class_name:
            windows.append(hwnd)
        return True

    u.EnumWindows(visit, 0)
    return windows[0] if windows else None


def wait_for(predicate, process, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        assert process.poll() is None, "Viewer exited unexpectedly"
        time.sleep(0.01)
    raise AssertionError("Timed out waiting for association test")


def registry_snapshot():
    # Include both app registration and the protected user choice.
    result = {}

    def visit(path):
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path) as key:
                children, values, _ = winreg.QueryInfoKey(key)
                result[path] = [winreg.EnumValue(key, index) for index in range(values)]
                for index in range(children):
                    visit(path + "\\" + winreg.EnumKey(key, index))
        except FileNotFoundError:
            result[path] = None

    for path in (
        r"Software\Classes\Voltura.Markdown",
        r"Software\Voltura\Markdown\Capabilities",
        r"Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.md\UserChoice",
        r"Software\RegisteredApplications",
        r"Software\Classes\.md",
    ):
        visit(path)
    return result


before = registry_snapshot()
summary = []
for mode in ("mismatch", "copy-failure", "unknown", "slow", "defer", "empty", "production"):
    metric = folder / (mode + ".jsonl")
    executable = root / "build/Release/vmd-test.exe"
    if mode == "mismatch":
        relocated = folder / "Portable copy Å" / "reader renamed.exe"
        relocated.parent.mkdir()
        shutil.copy2(executable, relocated)
        executable = relocated
    args = [str(executable), "--no-preferences", "--metrics", str(metric)]
    if mode != "empty":
        args.append(str(root / "tests/showcase.md"))
    if mode != "production":
        args += ["--test-association", "mismatch" if mode == "empty" else mode]
    process = subprocess.Popen(args)
    hwnd = None

    def rows():
        if not metric.exists():
            return []
        # A row may still be in flight while reading the append-only log.
        return [
            json.loads(line)
            for line in metric.read_text(encoding="utf-8").splitlines(keepends=True)
            if line.endswith("\n")
        ]

    def event(name):
        return next((row for row in rows() if row.get("association") == name), None)

    try:
        hwnd = wait_for(lambda: window_for(process.pid), process)
        delayed = wait_for(lambda: event("delay_started"), process)
        if mode != "empty":
            ready = next(row for row in rows() if "first_content_ms" in row)
            assert ready["first_content_ms"] <= delayed["elapsed_ms"]
            assert not any(row.get("association") == "probe_started" for row in rows())
        probe = wait_for(lambda: event("probe_started"), process)
        delay_rows = [row for row in rows() if row.get("association") == "delay_started"]
        assert probe["elapsed_ms"] - delay_rows[-1]["elapsed_ms"] >= 2990
        if mode == "defer":
            assert event("deferred") and len(delay_rows) == 2
            assert probe["generation"] == 2
            assert len([row for row in rows() if "first_content_ms" in row]) == 2
        if mode == "slow":
            # Exercise input while the probe is deliberately stalled for 15 seconds.
            for command in (103, 102, 104, 125, 106):
                u.SendMessageW(hwnd, 0x111, command, 0)
            assert not event("probe_finished")
            close_started = time.monotonic()
            u.PostMessageW(hwnd, 0x10, 0, 0)
            process.wait(timeout=2)
            assert time.monotonic() - close_started < 1
        else:
            wait_for(lambda: event("probe_finished"), process)
            panel = window_for(process.pid, "VolturaMarkdownAssociation")
            if mode == "unknown":
                assert not panel and not event("notice_shown")
            elif mode != "production":
                wait_for(lambda: event("notice_shown"), process)
                panel = window_for(process.pid, "VolturaMarkdownAssociation")
                assert panel and u.IsWindowVisible(panel)
                # Measure focus across notice creation inside the viewer, not across
                # the three-second delay when desktop focus can legitimately change.
                assert event("notice_focus_preserved"), "Notice stole keyboard focus"
                action, copy, dismiss = (u.GetDlgItem(panel, n) for n in (150, 152, 151))
                # Tab must reach all buttons through the existing application tab loop.
                u.PostMessageW(hwnd, 0x100, 9, 0)
                time.sleep(0.05)
                for _ in range(20):
                    current = focus(hwnd)
                    if current == action:
                        break
                    u.PostMessageW(current or hwnd, 0x100, 9, 0)
                    time.sleep(0.03)
                assert focus(hwnd) == action, "Notice action is unreachable by Tab"
                u.PostMessageW(action, 0x100, 9, 0)
                wait_for(lambda: focus(hwnd) == copy, process)
                original_bounds = bounds(panel)
                # Test-only clipboard sink keeps the user's clipboard untouched.
                u.PostMessageW(copy, 0x100, 13, 0)
                expected = "Copy failed" if mode == "copy-failure" else "Copied"
                wait_for(lambda: label(copy) == expected, process)
                copied = next(row["copied_executable"] for row in rows() if "copied_executable" in row)
                assert copied == str(executable.resolve()), "Must copy the running executable, not document"
                assert bounds(panel) == original_bounds, "Copy feedback expanded the notice"
                assert not event("registration_started"), "Copy must not register or open Settings"
                assert focus(hwnd) == copy and u.IsWindowVisible(panel)
                u.PostMessageW(copy, 0x100, 9, 0)
                wait_for(lambda: focus(hwnd) == dismiss, process)
                wait_for(lambda: label(copy) == "Copy path", process)
                for theme in ("alternate", "original"):
                    u.SendMessageW(hwnd, 0x111, 103, 0)
                    if mode == "mismatch" and importlib.util.find_spec("PIL"):
                        subprocess.run(
                            [
                                sys.executable,
                                str(root / "scripts/capture-window.py"),
                                str(process.pid),
                                str(folder / (theme + ".png")),
                            ],
                            check=True,
                        )
                # Test mode forces action failure without registry writes or Settings UI.
                u.PostMessageW(action, 0x100, 13, 0)
                wait_for(lambda: event("registration_failed"), process)
                assert u.IsWindowEnabled(action) and u.IsWindowVisible(panel)
                u.SendMessageW(dismiss, 0xF5, 0, 0)
                assert not u.IsWindowVisible(panel)
                u.SendMessageW(hwnd, 0x111, 102, 0)
                time.sleep(0.2)
                assert not u.IsWindowVisible(panel)
        assert len([row for row in rows() if row.get("association") == "probe_started"]) == 1
        summary.append({"mode": mode, "passed": True})
        print("PASS:", mode, flush=True)
    finally:
        if process.poll() is None:
            if hwnd:
                u.PostMessageW(hwnd, 0x10, 0, 0)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
assert before == registry_snapshot(), "Association tests changed real registration or defaults"
(folder / "results.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
print(folder)
