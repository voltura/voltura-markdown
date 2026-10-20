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
options.add_argument(
    "--print-dialogs",
    action="store_true",
    help="Also check print dialogs and PDF output; requires Microsoft Print to PDF",
)
args = options.parse_args()
if args.print_dialogs:
    os.environ["VMD_TEST_PRINT_TO_PDF"] = "1"
root = pathlib.Path(__file__).resolve().parents[1]
executable = pathlib.Path(os.environ.get("VMD_TEST_EXE", root / "build/Release/vmd-test.exe"))
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
u.GetDpiForWindow.argtypes = [w.HWND]
u.GetDpiForWindow.restype = w.UINT


def resize_window(width, height):
    scale = u.GetDpiForWindow(hwnd) / 96
    u.SetWindowPos(hwnd, None, 80, 80, round(width * scale), round(height * scale), 0x0014)


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
    modal = [
        (
            text(h),
            [
                (class_name(child), u.GetDlgCtrlID(child), text(child), bool(u.IsWindowVisible(child)))
                for child in windows(process.pid, h)
            ],
        )
        for h in windows(process.pid)
        if class_name(h) == "#32770"
    ]
    raise RuntimeError(f"Timed out: {description}; windows={top}; controls={details}; dialogs={modal}")


def dialog(title):
    if title in ("Create Markdown file", "File changed", "Unsaved Markdown"):
        required = 6
    elif title in ("Find", "Replace", "Print", "Print / PDF"):
        required = 2
    else:
        required = 1
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


def check_scrollbar_clipping(edit):
    g = c.WinDLL("gdi32")
    g.CreateRectRgn.argtypes = [c.c_int] * 4
    g.CreateRectRgn.restype = w.HANDLE
    g.PtInRegion.argtypes = [w.HANDLE, c.c_int, c.c_int]
    g.PtVisible.argtypes = [w.HDC, c.c_int, c.c_int]
    g.DeleteObject.argtypes = [w.HANDLE]
    u.GetWindowRgn.argtypes = [w.HWND, w.HANDLE]
    u.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
    u.GetWindowDC.argtypes = [w.HWND]
    u.GetWindowDC.restype = w.HDC
    u.ReleaseDC.argtypes = [w.HWND, w.HDC]
    region = g.CreateRectRgn(0, 0, 0, 0)
    try:
        client = w.RECT()
        u.GetClientRect(edit, c.byref(client))
        assert u.GetWindowRgn(edit, region), "Source must have an explicit visible region"
        assert g.PtInRegion(region, client.right // 2, client.bottom // 2), "Source text remains visible"
        assert not g.PtInRegion(region, client.right, client.bottom // 2), "Native vertical bar is excluded"
        assert not g.PtInRegion(region, client.right // 2, client.bottom), "Native horizontal bar is excluded"
        dc = u.GetWindowDC(edit)
        try:
            assert dc, "Source window drawing context"
            assert not g.PtVisible(
                dc, client.right, client.bottom // 2
            ), "Native DC cannot paint vertical bar"
            assert not g.PtVisible(
                dc, client.right // 2, client.bottom
            ), "Native DC cannot paint horizontal bar"
        finally:
            if dc:
                u.ReleaseDC(edit, dc)
    finally:
        g.DeleteObject(region)


def capture(name, target=None):
    if not args.capture:
        return
    time.sleep(0.1)  # Let native modal initialization and its first paint finish.
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
    target = target or hwnd
    rect = w.RECT()
    u.GetWindowRect(target, c.byref(rect))
    width, height = rect.right - rect.left, rect.bottom - rect.top
    dc = u.GetWindowDC(target)
    memory = g.CreateCompatibleDC(dc)
    bitmap = g.CreateCompatibleBitmap(dc, width, height)
    previous = g.SelectObject(memory, bitmap)
    try:
        assert u.PrintWindow(target, memory, 2)
        buffer = c.create_string_buffer(width * height * 4)
        g.GetBitmapBits.argtypes = [w.HANDLE, w.LONG, c.c_void_p]
        assert g.GetBitmapBits(bitmap, len(buffer), buffer)
        Image.frombytes("RGB", (width, height), buffer.raw, "raw", "BGRX").save(folder / name)
    finally:
        g.SelectObject(memory, previous)
        g.DeleteObject(bitmap)
        g.DeleteDC(memory)
        u.ReleaseDC(target, dc)


def check_dialog_theme(target, dark):
    u.GetPropW.argtypes = [w.HWND, w.LPCWSTR]
    u.GetPropW.restype = w.HANDLE
    wait(lambda: u.GetPropW(target, "VolturaDialogColors"), "dialog theme attached")
    dwm = c.WinDLL("dwmapi")
    dwm.DwmGetWindowAttribute.argtypes = [w.HWND, w.DWORD, c.c_void_p, w.DWORD]
    night = w.BOOL()
    assert dwm.DwmGetWindowAttribute(target, 20, c.byref(night), c.sizeof(night)) == 0
    assert bool(night.value) == dark


for accept in (False, True):
    path = folder / ("new å.md" if accept else "declined.md")
    metrics = folder / ("accept.jsonl" if accept else "decline.jsonl")
    process = subprocess.Popen([str(executable), str(path), "--no-preferences", "--metrics", str(metrics)])
    hwnd = None
    try:
        hwnd = wait(
            lambda: next((h for h in windows(process.pid) if class_name(h) == "VolturaMarkdownWindow"), None),
            "reader window",
        )
        choice = dialog("Create Markdown file")
        check_dialog_theme(choice, True)
        capture("create-dark.png", choice)
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
        resize_window(1100, 780)
        edit = u.GetDlgItem(panel, 100)
        canvas = next(h for h in windows(process.pid, hwnd) if class_name(h) == "VolturaMarkdownCanvas")
        wait(lambda: u.IsWindowVisible(canvas) and u.IsWindowVisible(edit), "default wide live preview")
        assert text(u.GetDlgItem(panel, 12)) == "Live: on"
        assert not path.exists(), "Create must defer disk writes until Save"
        navigation_source = (
            "[Jump](#%64estination-1)\n\n## Destination\n\n" + "Paragraph before destination.\n\n" * 200
        )
        destination_markup = "<table><tr><td><h2>Destination</h2></td></tr></table>"
        navigation_source += destination_markup + "\n\n" + "Paragraph after destination.\n\n" * 80
        set_text(edit, navigation_source)
        wait(lambda: "0 warnings" in text(u.GetDlgItem(panel, 9)), "navigation fixture validation")
        time.sleep(0.7)
        u.SendMessageW(edit, 0x00B6, 0, -u.SendMessageW(edit, 0x00CE, 0, 0))
        wait(lambda: u.SendMessageW(hwnd, 0x8028, 0, 0) == 1, "source drives preview to link")
        scale = u.GetDpiForWindow(hwnd) / 96
        point = round(40 * scale) | (round(8 * scale) << 16)
        u.SendMessageW(canvas, 0x0201, 1, point)
        u.SendMessageW(canvas, 0x0202, 0, point)
        destination_line = u.SendMessageW(edit, 0x0436, 0, navigation_source.rindex(destination_markup))
        wait(
            lambda: u.SendMessageW(edit, 0x00CE, 0, 0) >= destination_line - 1,
            "preview heading link moves source to destination",
        )
        time.sleep(0.5)  # Allow lazy look-ahead to run after navigation.
        assert (
            u.SendMessageW(edit, 0x00CE, 0, 0) >= destination_line - 1
        ), "Heading navigation must not snap back"
        print("PASS: encoded duplicate-heading links reach the second heading and survive lazy layout")
        scroll_image = folder / "scroll.svg"
        scroll_image.write_text(
            '<svg xmlns="http://www.w3.org/2000/svg" width="400" height="1200">'
            '<rect width="400" height="1200" fill="blue"/></svg>',
            encoding="utf-8",
        )
        footer_tables = (
            "<table>\n<tfoot><tr><td>Footer without spans</td></tr></tfoot>\n"
            "<tbody><tr><td>Body without spans</td></tr></tbody>\n</table>\n"
            '<table>\n<tfoot><tr><td colspan="2">Footer with spans</td></tr></tfoot>\n'
            '<tbody><tr><td colspan="2">Body with spans</td></tr></tbody>\n</table>\n'
        )
        scroll_source = (
            "# Scroll å\n\n"
            + (
                f"![]({scroll_image.as_posix()})\n\n"
                '<div data-example="<p>Hidden attribute</p>">\n<!--\n<p>Hidden comment</p>\n'
                + "Hidden source line å.\n" * 120
                + "-->\n<table>\n"
                + '<tbody><tr><td colspan="2">Before HTML image</td></tr></tbody>\n' * 20
                + "</table>\n"
                + footer_tables
                + f'<p>HTML caption</p>\n<img src="{scroll_image.as_posix()}">\n</div>\n\n'
            )
            * 5
            + "## Aligned section\n\n"
            + "Paragraph after images.\n\n" * 80
        )
        set_text(edit, scroll_source)
        wait(lambda: "0 warnings" in text(u.GetDlgItem(panel, 9)), "scroll fixture validation")
        time.sleep(0.7)
        for marker in ("<tbody>", "<tfoot>", "<p>HTML caption"):
            position = scroll_source.index(marker)
            target_line = u.SendMessageW(edit, 0x0436, 0, position)
            u.SendMessageW(edit, 0x00B6, 0, target_line - u.SendMessageW(edit, 0x00CE, 0, 0))
            wait(
                lambda: abs(u.SendMessageW(hwnd, 0x8028, 0, 0) - 1 - position) < 3,
                "preview anchors ignore hidden markup and retain table row-group locations",
            )
        section = scroll_source.index("## Aligned section")
        line = u.SendMessageW(edit, 0x0436, 0, section)
        u.SendMessageW(edit, 0x00B6, 0, line - u.SendMessageW(edit, 0x00CE, 0, 0))
        wait(
            lambda: abs(u.SendMessageW(hwnd, 0x8028, 0, 0) - 1 - section) < 3,
            "source section aligns after tall Markdown and HTML images",
        )
        for direction in (0, 1, 0):  # Native preview scrollbar scrolls both panes.
            u.SendMessageW(hwnd, 0x0115, direction, 0)
            time.sleep(0.1)
        assert u.SendMessageW(edit, 0x00CE, 0, 0) < line, "preview scrolling moves source backwards"
        u.SendMessageW(hwnd, 0x0115, 6, 0)  # SB_TOP
        wait(lambda: u.SendMessageW(edit, 0x00CE, 0, 0) == 0, "preview returns source to top")
        print(
            "PASS: source and preview align across hidden markup, reordered footers, table bands, and tall images in both directions"
        )
        wrapped_source = (
            "# Wrapped source\n\n"
            + ("A long protocol paragraph with `inline code` and readable words. " * 30 + "\n\n") * 8
            + "### AI Assistant\n\n"
            + "Following paragraph.\n\n" * 80
        )
        set_text(edit, wrapped_source)
        time.sleep(0.8)
        wrapped_lines = u.SendMessageW(edit, 0x00BA, 0, 0)
        assert wrapped_lines > wrapped_source.count("\n") + 40, "split source must wrap long paragraphs"
        assert text(edit) == wrapped_source, "soft wrapping must not alter source"
        heading = wrapped_source.index("### AI Assistant")
        heading_line = u.SendMessageW(edit, 0x0436, 0, heading)
        u.SendMessageW(edit, 0x00B6, 0, heading_line - u.SendMessageW(edit, 0x00CE, 0, 0))
        wait(lambda: abs(u.SendMessageW(hwnd, 0x8028, 0, 0) - 1 - heading) < 3,
             "wrapped source heading aligns with preview")
        for direction in (0, 1, 0):
            u.SendMessageW(hwnd, 0x0115, direction, 0)
            time.sleep(0.1)
        assert u.SendMessageW(edit, 0x00CE, 0, 0) < heading_line
        command(panel, 12)
        wait(lambda: u.SendMessageW(edit, 0x00BA, 0, 0) == wrapped_source.count("\n") + 1,
             "full-width source restores unwrapped lines")
        command(panel, 12)
        wait(lambda: u.SendMessageW(edit, 0x00BA, 0, 0) == wrapped_lines, "split source wraps again")
        assert text(edit) == wrapped_source, "view changes preserve source"
        capture("wrapped-source.png")
        print("PASS: split source wraps long paragraphs, synchronizes both ways, and preserves text")
        set_text(edit, scroll_source)
        time.sleep(0.8)
        for mode in ("dark", "light"):
            if mode == "light":
                command(hwnd, 103)
                time.sleep(0.15)
            # Let lazy layouts settle before reaching the final bottom boundary.
            for settle in range(4):
                u.SendMessageW(hwnd, 0x0115, 7, 0)  # SB_BOTTOM
                time.sleep(0.3)
            bottom_position = u.SendMessageW(hwnd, 0x8028, 0, 0)
            u.SendMessageW(edit, 0x0115, 6, 0)  # Source SB_TOP.
            wait(lambda: u.SendMessageW(hwnd, 0x8028, 0, 0) == 1, "source returns preview to top")
            u.SendMessageW(edit, 0x0115, 7, 0)  # Source SB_BOTTOM.
            wait(
                lambda: u.SendMessageW(hwnd, 0x8028, 0, 0) == bottom_position,
                f"{mode} source bottom reaches preview bottom across tall images and tables",
            )
            for step in range(5):
                u.SendMessageW(edit, 0x020A, ((-120) & 0xFFFF) << 16, 0)
                time.sleep(0.03)
                assert u.SendMessageW(hwnd, 0x8028, 0, 0) == bottom_position, (
                    f"{mode} source wheel moved preview away from bottom"
                )
            bottom_samples = []
            for step in range(30):
                u.SendMessageW(canvas, 0x020A, ((-120) & 0xFFFF) << 16, 0)
                for sample in range(20):
                    time.sleep(0.01)
                    position = u.SendMessageW(hwnd, 0x8028, 0, 0)
                    bottom_samples.append(position)
            assert all(position == bottom_position for position in bottom_samples), (
                f"{mode} preview moved at bottom: {bottom_position} -> "
                f"{min(bottom_samples)}..{max(bottom_samples)} (last {bottom_samples[-1]})"
            )
            for step in range(80):
                # Exercise both source-driven and preview-driven rapid scrolling.
                u.SendMessageW(hwnd, 0x0115, 1 if step % 20 < 10 else 0, 0)
                delta = -120 if step % 20 < 10 else 120
                u.SendMessageW(edit, 0x020A, (delta & 0xFFFF) << 16, 0)
                check_scrollbar_clipping(edit)
            capture(f"scrollbar-{mode}.png")
        command(hwnd, 103)
        time.sleep(0.15)
        print("PASS: native scrollbar areas stay excluded during rapid scrolling in both themes")
        source = "# Hello å\n\nText and [broken](#missing).\n"
        set_text(edit, source)
        wait(lambda: "1 warning" in text(u.GetDlgItem(panel, 9)), "live validation")
        assert u.SendMessageW(hwnd, 0x0011, 0, 1) == 0, "dirty draft must veto update restart"
        command(panel, 9)
        time.sleep(0.1)
        capture("editor-dark.png")
        for action, title in ((51, "Replace"),):
            command(panel, action)
            find = dialog(title)
            check_dialog_theme(find, True)
            set_text(u.GetDlgItem(find, 0x480), "Hello")
            if title == "Replace":
                set_text(u.GetDlgItem(find, 0x481), "World")
            capture(title.lower() + "-dark.png", find)
            command(hwnd, 103)
            time.sleep(0.1)
            check_dialog_theme(find, False)
            capture(title.lower() + "-light.png", find)
            command(hwnd, 103)
            if title == "Replace":
                replace_all = next(
                    h for h in windows(process.pid, find) if text(h).replace("&", "") == "Replace All"
                )
                u.PostMessageW(replace_all, 0x00F5, 0, 0)  # BM_CLICK
                wait(lambda: "World" in text(edit), "themed Replace All")
                u.SendMessageW(edit, 0x00C7, 0, 0)
                wait(lambda: text(edit) == source, "themed Replace All undo")
            command(find, 2)
            wait(lambda: not u.IsWindowVisible(find), "close find dialog")
        print("PASS: Replace follows dark/light theme while open")
        command(panel, 3)
        unsaved = dialog("Unsaved Markdown")
        check_dialog_theme(unsaved, True)
        capture("unsaved-dark.png", unsaved)
        command(unsaved, 2)
        for night in (True, False):
            if not night:
                command(hwnd, 103)
                time.sleep(0.1)
            pickers = [(54, "Open"), (20, "Save As")]
            if args.print_dialogs:
                pickers.append((60, "Print"))
            for action, title in pickers:
                command(panel, action)
                picker = dialog(title)
                if title == "Open":
                    wait(
                        lambda: any(
                            class_name(h) == "SHELLDLL_DefView"
                            for h in windows(process.pid, picker)
                        ),
                        "Explorer-style Open dialog",
                    )
                    assert not u.GetPropW(picker, "VolturaDialogColors")
                else:
                    check_dialog_theme(picker, night)
                mode = "dark" if night else "light"
                capture(title.lower().replace(" ", "-") + "-" + mode + ".png", picker)
                if title == "Print":
                    cancel = next(
                        h for h in windows(process.pid, picker) if text(h).replace("&", "") == "Cancel"
                    )
                    u.PostMessageW(cancel, 0x00F5, 0, 0)
                else:
                    command(picker, 2)
                wait(lambda: not u.IsWindowVisible(picker), "cancel " + title)
        command(hwnd, 103)
        print("PASS: Explorer-style Open and themed save/print pickers in both app themes")
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
        check_dialog_theme(table, False)
        capture("table-light.png", table)
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
        for night in (True, False):
            command(hwnd, 103)
            command(panel, 15)
            image_dialog = dialog("Insert image")
            check_dialog_theme(image_dialog, night)
            capture("image-" + ("dark" if night else "light") + ".png", image_dialog)
            command(image_dialog, 2)
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
        resize_window(720, 780)
        u.GetWindowRect(edit, c.byref(source_rect))
        u.GetWindowRect(canvas, c.byref(preview_rect))
        assert source_rect.bottom < preview_rect.top, "Narrow live preview must stack"
        assert u.IsWindowVisible(u.GetDlgItem(panel, 10)), "Narrow toolbar needs overflow"
        resize_window(1100, 780)
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
        if args.print_dialogs:
            command(panel, 60)
            print_dialog = dialog("Print")
            assert any(
                "Microsoft Print to PDF" in text(h) for h in windows(process.pid, print_dialog)
            ), "PDF test printer must be selected before printing"
            check_dialog_theme(print_dialog, False)
            command(print_dialog, 1)
            pdf_dialog = dialog("Save As")
            check_dialog_theme(pdf_dialog, False)
            pdf_path = folder / "themed-print.pdf"
            set_text(u.GetDlgItem(pdf_dialog, 0x480), str(pdf_path))
            command(pdf_dialog, 1)
            completed = dialog("Print / PDF")
            assert pdf_path.read_bytes().startswith(b"%PDF") and pdf_path.stat().st_size > 1000
            command(completed, 2)
            print("PASS: themed print selection and PDF destination produce a valid PDF")
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
        guarded_path = folder / "overwrite-guard.md"
        guarded_path.write_text("keep", encoding="utf-8")
        set_text(name_edit, str(guarded_path))
        command(save_dialog, 1)
        overwrite = wait(
            lambda: next(
                (
                    h
                    for h in windows(process.pid)
                    if class_name(h) == "#32770"
                    and u.IsWindowVisible(u.GetDlgItem(h, 6))
                    and u.IsWindowVisible(u.GetDlgItem(h, 7))
                ),
                None,
            ),
            "native overwrite confirmation",
        )
        check_dialog_theme(overwrite, False)
        capture("overwrite-light.png", overwrite)
        command(overwrite, 7)
        save_dialog = dialog("Save As")
        name_edit = u.GetDlgItem(save_dialog, 0x480)
        assert guarded_path.read_text(encoding="utf-8") == "keep"
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
        unsaved = dialog("Unsaved Markdown")
        check_dialog_theme(unsaved, False)
        capture("unsaved-light.png", unsaved)
        command(unsaved, 2)
        time.sleep(0.1)
        assert editor_window(), "Cancel must preserve editing"
        command(hwnd, 164)
        command(dialog("Unsaved Markdown"), 7)
        wait(lambda: not editor_window(), "discard draft")
        wait(
            lambda: u.SendMessageW(hwnd, 0x8029, 0, 0) == 1,
            "reader rendering buffer matches canvas after leaving live edit",
        )
        assert "discard me" not in second_path.read_text(encoding="utf-8")
        for window_width in (1100, 720):
            resize_window(window_width, 780)
            command(hwnd, 161)
            panel = wait(editor_window, "reopen saved document for scaling regression")
            edit = u.GetDlgItem(panel, 100)
            if not u.IsWindowVisible(edit):
                command(panel, 2)
            if text(u.GetDlgItem(panel, 12)) != "Live: on":
                command(panel, 12)
            wait(lambda: u.IsWindowVisible(canvas), "live preview before returning to reader")
            time.sleep(0.3)  # Let the posted preview layout and paint finish before leaving.
            u.GetWindowRect(canvas, c.byref(preview_rect))
            u.GetWindowRect(hwnd, c.byref(source_rect))
            if window_width == 1100:
                assert preview_rect.right - preview_rect.left < source_rect.right - source_rect.left - 100
            else:
                u.GetWindowRect(edit, c.byref(source_rect))
                assert source_rect.bottom < preview_rect.top, "Compact live preview must be stacked"
            wait(lambda: u.SendMessageW(hwnd, 0x8029, 0, 0) == 1, "preview rendering buffer matches canvas")
            command(hwnd, 164)
            wait(lambda: not editor_window(), "leave live editing without resizing parent")
            wait(
                lambda: u.SendMessageW(hwnd, 0x8029, 0, 0) == 1,
                "reader rendering buffer matches expanded canvas",
            )
        print("PASS: leaving side-by-side and stacked live editing keeps reader scaling correct")
        # Reader mode stays non-editable; Ctrl+N equivalent menu action creates untitled drafts.
        command(hwnd, 160)
        panel = wait(editor_window, "untitled draft")
        assert "Untitled" in text(hwnd)
        command(hwnd, 164)
        wait(lambda: not editor_window(), "finish clean untitled draft")
        resize_window(720, 780)
        command(hwnd, 160)
        panel = wait(editor_window, "compact untitled draft")
        wait(lambda: text(u.GetDlgItem(panel, 12)) == "Live", "compact source-first mode")
        wait(lambda: not u.IsWindowVisible(canvas), "compact source/preview toggle")
        command(hwnd, 164)
        wait(lambda: not editor_window(), "finish compact untitled draft")
        wait(
            lambda: u.SendMessageW(hwnd, 0x8029, 0, 0) == 1,
            "reader rendering buffer matches canvas after leaving compact edit",
        )
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
process = subprocess.Popen([str(executable), "--no-preferences"], env=environment)
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
process = subprocess.Popen([str(executable), "--no-preferences"], env=environment)
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
