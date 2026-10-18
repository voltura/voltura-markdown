"""Exercise native editing, helpers, safe saves, and missing-file startup on a private desktop."""

import argparse
import ctypes as c
from ctypes import wintypes as w
import pathlib
import json
import subprocess
import time
import uuid
import os

from test_desktop import isolate

isolate()
options = argparse.ArgumentParser(description=__doc__)
options.add_argument("--capture", action="store_true")
args = options.parse_args()
root = pathlib.Path(__file__).resolve().parents[1]
folder = root / "build" / ("edit-test-" + uuid.uuid4().hex)
folder.mkdir()
u = c.WinDLL("user32", use_last_error=True)
u.SetProcessDpiAwarenessContext(c.c_void_p(-4))
callback = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
for name in ("EnumWindows", "EnumChildWindows"):
    getattr(u, name).argtypes = ([w.HWND] if name == "EnumChildWindows" else []) + [callback, w.LPARAM]
u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
u.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
u.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
u.GetWindowTextLengthW.argtypes = [w.HWND]
u.GetDlgItem.argtypes = [w.HWND, c.c_int]
u.GetDlgItem.restype = w.HWND
u.GetDlgCtrlID.argtypes = [w.HWND]
u.SendMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.SendMessageW.restype = c.c_ssize_t
u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.IsWindowVisible.argtypes = [w.HWND]
u.IsWindowEnabled.argtypes = [w.HWND]
u.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
u.SetWindowPos.argtypes = [w.HWND, w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.UINT]


def class_name(hwnd):
    value = c.create_unicode_buffer(128)
    u.GetClassNameW(hwnd, value, len(value))
    return value.value


def text(hwnd):
    value = c.create_unicode_buffer(u.SendMessageW(hwnd, 0x000E, 0, 0) + 2)
    u.SendMessageW(hwnd, 0x000D, len(value), c.cast(value, c.c_void_p).value)
    return value.value.replace("\r\n", "\n").replace("\r", "\n")


def set_text(hwnd, value):
    buffer = c.create_unicode_buffer(value)
    u.SendMessageW(hwnd, 0x000C, 0, c.cast(buffer, c.c_void_p).value)


def windows(pid, parent=None):
    result = []

    @callback
    def visit(hwnd, unused):
        owner = w.DWORD()
        u.GetWindowThreadProcessId(hwnd, c.byref(owner))
        if owner.value == pid:
            result.append(hwnd)
        return True

    if parent:
        u.EnumChildWindows(parent, visit, 0)
    else:
        u.EnumWindows(visit, 0)
    return result


def wait(predicate, description):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        if process.poll() is not None:
            raise RuntimeError(f"App exited during {description}")
        time.sleep(0.01)
    details = (
        [(class_name(h), u.GetDlgCtrlID(h), text(h)[:150]) for h in windows(process.pid, hwnd)]
        if hwnd
        else []
    )
    top = [(class_name(h), text(h)[:300]) for h in windows(process.pid)]
    raise RuntimeError(f"Timed out: {description}; windows={top}; controls={details}")


def dialog(title):
    required = 6 if title in ("Create Markdown file", "File changed", "Unsaved Markdown") else 1
    return wait(
        lambda: next(
            (
                h
                for h in windows(process.pid)
                if class_name(h) == "#32770"
                and title in text(h)
                and u.IsWindowVisible(u.GetDlgItem(h, required))
                and u.IsWindowEnabled(u.GetDlgItem(h, required))
            ),
            None,
        ),
        title,
    )


def command(hwnd, action):
    u.PostMessageW(hwnd, 0x0111, action, 0)


def editor_window():
    return next((h for h in windows(process.pid, hwnd) if class_name(h) == "VolturaMarkdownEditor"), None)


def capture(name):
    if not args.capture:
        return
    from PIL import Image

    g = c.WinDLL("gdi32")
    for function in ("CreateCompatibleDC", "CreateCompatibleBitmap", "SelectObject"):
        getattr(g, function).restype = w.HANDLE
    g.CreateCompatibleDC.argtypes = [w.HDC]
    g.CreateCompatibleBitmap.argtypes = [w.HDC, c.c_int, c.c_int]
    g.SelectObject.argtypes = [w.HDC, w.HANDLE]
    g.DeleteObject.argtypes = [w.HANDLE]
    g.DeleteDC.argtypes = [w.HDC]
    u.GetWindowDC.argtypes = [w.HWND]
    u.GetWindowDC.restype = w.HDC
    u.ReleaseDC.argtypes = [w.HWND, w.HDC]
    u.PrintWindow.argtypes = [w.HWND, w.HDC, w.UINT]
    rect = w.RECT()
    u.GetWindowRect(hwnd, c.byref(rect))
    width, height = rect.right - rect.left, rect.bottom - rect.top
    dc = u.GetWindowDC(hwnd)
    memory = g.CreateCompatibleDC(dc)
    bitmap = g.CreateCompatibleBitmap(dc, width, height)
    previous = g.SelectObject(memory, bitmap)
    try:
        assert u.PrintWindow(hwnd, memory, 2)
        buffer = c.create_string_buffer(width * height * 4)
        g.GetBitmapBits.argtypes = [w.HANDLE, w.LONG, c.c_void_p]
        assert g.GetBitmapBits(bitmap, len(buffer), buffer)
        Image.frombytes("RGB", (width, height), buffer.raw, "raw", "BGRX").save(folder / name)
    finally:
        g.SelectObject(memory, previous)
        g.DeleteObject(bitmap)
        g.DeleteDC(memory)
        u.ReleaseDC(hwnd, dc)


for accept in (False, True):
    path = folder / ("new å.md" if accept else "declined.md")
    metrics = folder / ("accept.jsonl" if accept else "decline.jsonl")
    process = subprocess.Popen(
        [str(root / "build/Release/vmd-test.exe"), str(path), "--no-preferences", "--metrics", str(metrics)]
    )
    hwnd = None
    try:
        hwnd = wait(
            lambda: next((h for h in windows(process.pid) if class_name(h) == "VolturaMarkdownWindow"), None),
            "reader window",
        )
        choice = dialog("Create Markdown file")
        command(choice, 6 if accept else 7)
        if not accept:
            time.sleep(0.15)
            assert not path.exists() and not editor_window()
            u.PostMessageW(hwnd, 0x0010, 0, 0)
            process.wait(timeout=10)
            print("PASS: declined missing startup file leaves empty reader")
            continue
        panel = wait(editor_window, "new-file editor")
        u.PostMessageW(hwnd, 0x0100, 0x7A, 0)  # F11: inspect a normal-size window.
        time.sleep(0.1)
        u.SetWindowPos(hwnd, None, 80, 80, 1100, 780, 0x0014)
        edit = u.GetDlgItem(panel, 100)
        assert not path.exists(), "Create must defer disk writes until Save"
        source = "# Hello å\n\nText and [broken](#missing).\n"
        set_text(edit, source)
        wait(lambda: "1 warning" in text(u.GetDlgItem(panel, 9)), "live validation")
        assert u.SendMessageW(hwnd, 0x0011, 0, 1) == 0, "dirty draft must veto update restart"
        command(panel, 9)
        time.sleep(0.1)
        capture("editor-dark.png")
        command(hwnd, 103)
        time.sleep(0.15)
        capture("editor-light.png")
        # Make the whole insertion one undoable change, including a multiline helper.
        u.SendMessageW(edit, 0x00B1, 2, 7)
        command(panel, 5)
        wait(lambda: "**Hello**" in text(edit), "bold selection")
        u.SendMessageW(edit, 0x00C7, 0, 0)
        assert text(edit) == source
        u.SendMessageW(edit, 0x00B1, 2, 7)
        command(panel, 11)
        wait(lambda: "<u>Hello</u>" in text(edit), "underline selection")
        u.SendMessageW(edit, 0x00C7, 0, 0)
        assert text(edit) == source
        u.SendMessageW(edit, 0x00B1, len(source), len(source))
        command(panel, 30)
        table = dialog("Insert table")
        set_text(u.GetDlgItem(table, 201), "2")
        set_text(u.GetDlgItem(table, 202), "3")
        command(table, 1)
        wait(lambda: "| Header 1 | Header 2 | Header 3 |" in text(edit), "table insertion")
        command(panel, 7)
        link = dialog("Insert link")
        set_text(u.GetDlgItem(link, 201), "Home")
        set_text(u.GetDlgItem(link, 202), "https://voltura.se")
        command(link, 1)
        wait(lambda: "[Home](<https://voltura.se>)" in text(edit), "link insertion")
        image_path = folder / "image #1%20&copy; å.svg"
        image_path.write_text(
            '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24"><rect width="24" height="24" fill="blue"/></svg>',
            encoding="utf-8",
        )
        at = len(text(edit))
        u.SendMessageW(edit, 0x00B1, at, at)
        command(panel, 15)
        image_dialog = dialog("Insert image")
        set_text(u.GetDlgItem(image_dialog, 201), "Local image")
        set_text(u.GetDlgItem(image_dialog, 202), str(image_path))
        command(image_dialog, 1)
        wait(
            lambda: "![Local image](<image%20%231%2520%26copy;%20å.svg>)" in text(edit),
            "reserved-character image insertion",
        )
        at = len(text(edit))
        u.SendMessageW(edit, 0x00B1, at, at)
        before_placeholder = text(edit)
        command(panel, 15)
        image_dialog = dialog("Insert image")
        command(image_dialog, 204)
        wait(lambda: "![Image description](<path/to/image.png>)" in text(edit), "image placeholder")
        u.SendMessageW(edit, 0x00C7, 0, 0)
        assert text(edit) == before_placeholder
        draft = text(edit)
        command(hwnd, 162)
        wait(lambda: path.exists() and " *" not in text(hwnd), "first save")
        assert path.read_text(encoding="utf-8") == draft
        command(panel, 2)
        wait(lambda: not u.IsWindowVisible(edit), "draft preview")

        def preview_painted():
            if not metrics.exists():
                return False
            return any(
                row.get("edit_preview_painted") and row["source"] == 0 and row["first_kind"] == 1
                for row in (
                    json.loads(line) for line in metrics.read_text().splitlines() if line.endswith("}")
                )
            )

        wait(preview_painted, "formatted heading preview paint")
        time.sleep(0.2)
        capture("preview-light.png")
        command(panel, 2)
        wait(lambda: u.IsWindowVisible(edit), "return to source")
        assert text(edit) == draft
        command(panel, 12)
        canvas = next(h for h in windows(process.pid, hwnd) if class_name(h) == "VolturaMarkdownCanvas")
        wait(lambda: u.IsWindowVisible(canvas) and u.IsWindowVisible(edit), "live preview panes")
        live_source = draft + "\n## Live update\n"
        set_text(edit, live_source)

        def live_painted():
            return any(
                row.get("live") and row.get("source_focus") and row.get("source_units") == len(live_source)
                for row in (
                    json.loads(line) for line in metrics.read_text().splitlines() if line.endswith("}")
                )
            )

        wait(live_painted, "live revision painted without moving editor focus")
        source_rect, preview_rect = w.RECT(), w.RECT()
        u.GetWindowRect(edit, c.byref(source_rect))
        u.GetWindowRect(canvas, c.byref(preview_rect))
        assert source_rect.right < preview_rect.left, "Wide live preview must be side by side"
        capture("live-light.png")
        u.SetWindowPos(hwnd, None, 80, 80, 720, 780, 0x0014)
        u.GetWindowRect(edit, c.byref(source_rect))
        u.GetWindowRect(canvas, c.byref(preview_rect))
        assert source_rect.bottom < preview_rect.top, "Narrow live preview must stack"
        assert u.IsWindowVisible(u.GetDlgItem(panel, 10)), "Narrow toolbar needs overflow"
        u.SetWindowPos(hwnd, None, 80, 80, 1100, 780, 0x0014)
        command(panel, 12)
        wait(lambda: not u.IsWindowVisible(canvas), "disable live preview")
        command(hwnd, 125)
        wait(lambda: not u.IsWindowVisible(panel), "help while editing")
        command(hwnd, 161)
        wait(lambda: u.IsWindowVisible(panel) and " *" in text(hwnd), "return from help to modified editor")
        assert text(edit) == live_source
        u.SendMessageW(edit, 0x00B1, len(live_source), len(live_source))
        command(panel, 13)
        wait(lambda: "```\ncode\n```" in text(edit), "code toolbar helper")
        draft = text(edit)
        command(hwnd, 162)
        wait(lambda: " *" not in text(hwnd), "save live-edited source")
        # Save As uses the native chooser and leaves the old file intact.
        command(hwnd, 163)
        save_dialog = dialog("Save As")
        name_edit = wait(
            lambda: next(
                (
                    h
                    for h in windows(process.pid, save_dialog)
                    if class_name(h) == "Edit" and u.GetDlgCtrlID(h) in (1148, 1152, 1001)
                ),
                None,
            ),
            "native Save As filename field",
        )
        second_path = folder / "saved-as.md"
        set_text(name_edit, str(second_path))
        command(save_dialog, 1)
        wait(lambda: second_path.exists() and "saved-as.md" in text(hwnd), "Save As completion")

        def saved_paths_match():
            values = [
                row["edit_saved_path_matches"]
                for row in (
                    json.loads(line) for line in metrics.read_text().splitlines() if line.endswith("}")
                )
                if "edit_saved_path_matches" in row
            ]
            return len(values) >= 3 and all(values)

        wait(saved_paths_match, "file actions follow Save As without preview")
        assert path.read_text(encoding="utf-8") == draft
        set_text(edit, draft + "\nunsaved\n")
        second_path.write_text("external", encoding="utf-8")
        command(hwnd, 162)
        conflict = dialog("File changed")
        command(conflict, 2)
        time.sleep(0.1)
        assert second_path.read_text(encoding="utf-8") == "external" and "unsaved" in text(edit)
        command(hwnd, 162)
        command(dialog("File changed"), 6)
        wait(lambda: " *" not in text(hwnd), "explicit overwrite")
        assert "unsaved" in second_path.read_text(encoding="utf-8")
        set_text(edit, draft + "discard me")
        command(hwnd, 164)
        command(dialog("Unsaved Markdown"), 2)
        time.sleep(0.1)
        assert editor_window(), "Cancel must preserve editing"
        command(hwnd, 164)
        command(dialog("Unsaved Markdown"), 7)
        wait(lambda: not editor_window(), "discard draft")
        assert "discard me" not in second_path.read_text(encoding="utf-8")
        # Reader mode stays non-editable; Ctrl+N equivalent menu action creates untitled drafts.
        command(hwnd, 160)
        panel = wait(editor_window, "untitled draft")
        assert "Untitled" in text(hwnd)
        command(hwnd, 164)
        wait(lambda: not editor_window(), "finish clean untitled draft")
        assert (
            u.SendMessageW(hwnd, 0x0011, 0, 1) == 1
        ), "Clean untitled reader must allow manual-update restart"
        u.SendMessageW(hwnd, 0x0016, 0, 1)  # Cancel simulated session end and clean its snapshot.
        u.PostMessageW(hwnd, 0x0010, 0, 0)
        process.wait(timeout=10)
        print(
            "PASS: missing-file create, helpers, validation, live/toggle preview, save/Save As, conflict, discard/cancel, restart veto, new draft"
        )
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
# Sustained typing must checkpoint even without an idle pause. Keep all recovery data isolated.
profile = folder / "profile"
environment = {**os.environ, "LOCALAPPDATA": str(profile)}
recovery_dir = profile / "Voltura" / "Markdown" / "Recovery"
process = subprocess.Popen([str(root / "build/Release/vmd-test.exe"), "--no-preferences"], env=environment)
try:
    hwnd = wait(
        lambda: next((h for h in windows(process.pid) if class_name(h) == "VolturaMarkdownWindow"), None),
        "recovery test window",
    )
    command(hwnd, 160)
    panel = wait(editor_window, "recovery test editor")
    edit = wait(lambda: u.GetDlgItem(panel, 100), "recovery source control")
    wait(lambda: u.IsWindowEnabled(edit), "recovery source ready")
    for index in range(50):
        u.SendMessageW(edit, 0x0102, ord("x"), 1)
        time.sleep(0.08)
    assert text(edit) == "x" * 50, "Recovery test must type into the native source control"
    snapshots = list(recovery_dir.glob("*.draft"))
    assert snapshots, "Continuous typing must write recovery data before the first typing pause"
    process.kill()
    process.wait(timeout=10)
    recovered = subprocess.run(
        [str(root / "build/Release/edit_tests.exe"), "--list-recovery"],
        env=environment,
        capture_output=True,
        check=True,
    )
    assert b"xxxxxxxxxx" in recovered.stdout, "A crashed process must expose its checkpoint for recovery"
finally:
    if process.poll() is None:
        process.kill()
        process.wait(timeout=10)
print("PASS: continuous-typing recovery checkpoint and crashed-session discovery")

# A normal discard waits for owned recovery cleanup before process exit.
process = subprocess.Popen([str(root / "build/Release/vmd-test.exe"), "--no-preferences"], env=environment)
try:
    hwnd = wait(
        lambda: next((h for h in windows(process.pid) if class_name(h) == "VolturaMarkdownWindow"), None),
        "discard recovery test window",
    )
    command(hwnd, 160)
    panel = wait(editor_window, "discard recovery test editor")
    edit = wait(lambda: u.GetDlgItem(panel, 100), "discard source control")
    wait(lambda: u.IsWindowEnabled(edit), "discard source ready")
    set_text(edit, "discard recovery")
    wait(lambda: list(recovery_dir.glob(f"{process.pid}-*.draft")), "discard checkpoint")
    u.PostMessageW(hwnd, 0x0010, 0, 0)
    command(dialog("Unsaved Markdown"), 7)
    process.wait(timeout=10)
    assert not list(recovery_dir.glob(f"{process.pid}-*.draft")), "Discard must not reappear as recovery"
finally:
    if process.poll() is None:
        process.kill()
        process.wait(timeout=10)
print("PASS: normal discard drains recovery cleanup")
print(f"Artifacts: {folder}")
