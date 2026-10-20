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
options.add_argument("--editor", action="store_true")
options.add_argument("--navigation", action="store_true", help="Verify three widely separated matches are scrolled into view")
options.add_argument("--check-visibility", action="store_true", help="Check viewport visibility through all results (up to 100)")
options.add_argument("--executable", type=pathlib.Path, default=root / "build/Release/vmd-test.exe")
options.add_argument("--measure-only", action="store_true", help="Report first-hit, indexing and navigation timings without the behavior suite")
options.add_argument("--stress-count", action="store_true", help="Use 100,000 matches to exercise long counters")
args = options.parse_args()
folder = root / "build" / ("search-test-" + uuid.uuid4().hex)
folder.mkdir()
if args.navigation:
    args.query = "navigation-needle"
    args.document = folder / "navigation.md"
    args.document.write_text(("\n\n".join(f"## navigation-needle {n}\n\n" +
                                        ("A paragraph separating the results.\n\n" * 80)
                                        for n in range(3))), encoding="utf-8")
if args.stress_count:
    args.document = folder / "repeated.md"
    args.document.write_text((args.query + " ") * 100000, encoding="utf-8")
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
u.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
u.IsWindowVisible.argtypes = [w.HWND]
u.IsWindowEnabled.argtypes = [w.HWND]


class GuiThreadInfo(c.Structure):
    _fields_ = [("cbSize", w.DWORD), ("flags", w.DWORD)] + [
        (name, w.HWND) for name in ("active", "focus", "capture", "menu", "move", "caret")
    ] + [("caretRect", w.RECT)]


u.GetGUIThreadInfo.argtypes = [w.DWORD, c.POINTER(GuiThreadInfo)]


def assert_search_focus():
    info = GuiThreadInfo()
    info.cbSize = c.sizeof(info)
    assert u.GetGUIThreadInfo(u.GetWindowThreadProcessId(hwnd, None), c.byref(info))
    assert info.focus == edit, "Find must retain input focus"


def edit_bounds():
    bounds = w.RECT()
    u.GetWindowRect(edit, c.byref(bounds))
    return bounds.left, bounds.top, bounds.right, bounds.bottom


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
        str(args.executable.resolve()),
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
    if args.editor:
        u.SendMessageW(hwnd, 0x0111, 161, 0)
        time.sleep(0.5)
    u.SendMessageW(hwnd, 0x0111, 101, 0)
    edit = u.GetDlgItem(hwnd, 200)
    assert edit, "Search input exists"
    query = c.create_unicode_buffer(args.query)
    u.SendMessageW(edit, 0x000C, 0, c.cast(query, c.c_void_p).value)
    assert u.SendMessageW(edit, 0x000E, 0, 0) == len(args.query), "Setting query preserves input"
    total = wait_for(lambda: next((r for r in records() if "search_total" in r), None))
    first = next(r for r in records() if "search_match_paint_ms" in r)
    if first["count_started"] or total["search_total"] < 1:
        raise RuntimeError("Count must start after the first highlighted frame")
    time.sleep(0.1)
    before_paints = u.SendMessageW(hwnd, 0x8000 + 42, 0, 0)
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
    if args.measure_only:
        print(json.dumps({"first_match": first, "count": total, "next_match": paints[-1]}))
        print(f"Results: {folder}")
        raise SystemExit(0)
    time.sleep(0.1)
    assert u.SendMessageW(hwnd, 0x8000 + 42, 0, 0) == before_paints, "Counter updates must not repaint the input"
    original_bounds = edit_bounds()
    u.SendMessageW(edit, 0x00B1, len(args.query), len(args.query))  # EM_SETSEL
    u.SendMessageW(edit, 0x0102, ord("x"), 0)
    assert edit_bounds() == original_bounds, "Typing must not resize the input when the count clears"
    u.SendMessageW(edit, 0x0102, 8, 0)  # Backspace
    wait_for(lambda: len([r for r in records() if "search_total" in r]) == 2)
    assert edit_bounds() == original_bounds, "Completing a count must not resize the input"
    assert not u.GetDlgItem(hwnd, 201), "Find exposes only one close X"
    # Post real key messages so the application's Tab dispatch is exercised.
    for control_id in ([205, 202, 203, 204, 200] if args.editor else [202, 203, 204, 200]):
        u.PostMessageW(hwnd, 0x0100, 9, 0)
        def focus_is_expected():
            info = GuiThreadInfo()
            info.cbSize = c.sizeof(info)
            u.GetGUIThreadInfo(u.GetWindowThreadProcessId(hwnd, None), c.byref(info))
            return info.focus == u.GetDlgItem(hwnd, control_id)
        wait_for(focus_is_expected)
    def clear_query():
        u.SendMessageW(edit, 0x00B1, 0, -1)  # Ctrl+A equivalent
        u.SendMessageW(edit, 0x0102, 8, 0)  # Backspace
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
            # Exercise the clear action and real character input in each theme/layout.
            clear_query()
            assert_search_focus()
            assert u.SendMessageW(edit, 0x000E, 0, 0) == 0
            bounds = edit_bounds()
            previous_counts = len([r for r in records() if "search_total" in r])
            for character in args.query:
                u.SendMessageW(edit, 0x0102, ord(character), 0)
                assert edit_bounds() == bounds, "Typing keeps the field geometry stable"
            wait_for(lambda: len([r for r in records() if "search_total" in r]) > previous_counts)
            time.sleep(0.1)
            subprocess.run(
                [
                    "python",
                    str(root / "scripts/capture-window.py"),
                    str(process.pid),
                    str(folder / f"search-{mode}.png"),
                    "VolturaMarkdownWindow",
                    str(metrics),
                ],
                check=True,
            )
    clear_query()
    assert_search_focus()
    assert u.SendMessageW(edit, 0x000E, 0, 0) == 0, "Clear search empties the input"
    # Type and immediately clear before the debounce expires: old workers/timers
    # must not restore a highlight or count after clearing.
    u.SendMessageW(edit, 0x0102, ord("a"), 0)
    assert u.SendMessageW(edit, 0x000E, 0, 0) > 0, "Typing enters a new query"
    clear_query()
    counts_before = len([r for r in records() if "search_total" in r])
    time.sleep(0.3)
    assert len([r for r in records() if "search_total" in r]) == counts_before, "Clearing cancels pending search"
    u.SendMessageW(edit, 0x0102, ord("a"), 0)
    u.SendMessageW(edit, 0x0100, 0x1B, 0)  # Escape
    assert not u.IsWindowVisible(edit), "Closing Find hides input"
    u.SendMessageW(hwnd, 0x0111, 101, 0)
    assert u.SendMessageW(edit, 0x000E, 0, 0) > 0, "Reopening preserves the query"
    clear_query()
    def probe(which=0):
        return u.SendMessageW(hwnd, 0x802C, which, 0)
    query_value = c.create_unicode_buffer(args.query)
    u.SendMessageW(edit, 0x000C, 0, c.cast(query_value, c.c_void_p).value)
    wait_for(lambda: probe() > 0)
    if args.check_visibility and not args.editor:
        assert 1 < probe() <= 100
        for button in (203, 202):
            for unused in range(probe()):
                old_anchor = probe(1)
                u.SendMessageW(u.GetDlgItem(hwnd, button), 0x00F5, 0, 0)
                wait_for(lambda: probe(1) != old_anchor)
                wait_for(lambda: probe(8))
        print("PASS: every selected match is inside the document viewport in both directions")
    if args.navigation and not args.editor:
        assert probe() == 3
        wait_for(lambda: probe(8))
        first_anchor, first_scroll = probe(1), probe(7)
        for button, direction in ((203, 1), (203, 1), (203, -1), (202, 1), (202, -1), (202, -1)):
            old_anchor, old_scroll = probe(1), probe(7)
            u.SendMessageW(u.GetDlgItem(hwnd, button), 0x00F5, 0, 0)
            wait_for(lambda: probe(1) != old_anchor)
            wait_for(lambda: probe(8))
            assert (probe(7) - old_scroll) * direction > 0, "Arrow must scroll to the selected match"
        assert probe(1) == first_anchor
        print("PASS: arrows reveal separated matches in both directions and across wraparound")
    initial = probe(1)
    u.SendMessageW(u.GetDlgItem(hwnd, 203), 0x00F5, 0, 0)
    wait_for(lambda: len([row for row in records() if "search_match_paint_ms" in row]) > len(paints))
    time.sleep(0.1)
    assert_search_focus()
    u.SendMessageW(u.GetDlgItem(hwnd, 202), 0x00F5, 0, 0)
    wait_for(lambda: probe(1) == initial)
    assert probe(2) == probe(), "Index and displayed total agree"
    if not args.editor:
        for expected_source in (1, 0):
            code_button = u.GetDlgItem(hwnd, 102)
            u.SendMessageW(code_button, 0x0201, 1, (8 << 16) | 8)  # WM_LBUTTONDOWN
            u.SendMessageW(code_button, 0x0202, 0, (8 << 16) | 8)  # WM_LBUTTONUP
            assert probe(6) == expected_source, "Code button must toggle reader mode exactly once"
            wait_for(lambda: probe() > 0 and probe(2) == probe())
            assert probe(6) == expected_source, "Search completion must preserve the selected reader mode"
            assert_search_focus()
            if args.capture and expected_source:
                subprocess.run(["python", str(root / "scripts/capture-window.py"), str(process.pid),
                                str(folder / "search-source.png"), "VolturaMarkdownWindow", str(metrics)], check=True)
        for zoom_command in (142, 140):
            u.SendMessageW(hwnd, 0x0111, zoom_command, 0)
            assert probe(2) == probe(), "Zoom preserves the result index"
    missing = c.create_unicode_buffer("no-such-phrase-98a1")
    u.SendMessageW(edit, 0x000C, 0, c.cast(missing, c.c_void_p).value)
    wait_for(lambda: probe() == 0)
    assert probe(9), "No-match feedback is visible"
    for character in (ord("x"), 8, ord("y"), 8):
        u.SendMessageW(edit, 0x0102, character, 0)
        assert probe(9), "Typing must retain no-match feedback through debounce"
        deadline = time.monotonic() + 2
        while probe() != 0:
            assert probe(9), "Fast no-match search must not flash blank or progress text"
            assert time.monotonic() < deadline
            time.sleep(0.01)
        assert probe(9)
    assert not u.IsWindowEnabled(u.GetDlgItem(hwnd, 202))
    assert not u.IsWindowEnabled(u.GetDlgItem(hwnd, 203))
    if args.capture:
        subprocess.run(["python", str(root / "scripts/capture-window.py"), str(process.pid),
                        str(folder / "search-no-matches.png"), "VolturaMarkdownWindow", str(metrics)], check=True)
    u.SendMessageW(u.GetDlgItem(hwnd, 204), 0x00F5, 0, 0)
    assert not probe(4) and not probe(2) and not probe(3), "Closing removes all Find state"
    if args.editor:
        children = []
        @callback_type
        def child(window, unused):
            name = c.create_unicode_buffer(128)
            u.GetClassNameW(window, name, len(name))
            children.append((window, name.value))
            return True
        u.EnumChildWindows.argtypes = [w.HWND, callback_type, w.LPARAM]
        u.EnumChildWindows(hwnd, child, 0)
        source_edit = next(window for window, name in children if name == "RICHEDIT50W")
        panel = next(window for window, name in children if name == "VolturaMarkdownEditor")
        def set_value(window, value):
            value = c.create_unicode_buffer(value)
            u.SendMessageW(window, 0x000C, 0, c.cast(value, c.c_void_p).value)
        set_value(source_edit, "Alpha alpha alphabet\nångström ÅNGSTRÖM\n")
        u.SendMessageW(hwnd, 0x0111, 101, 0)
        set_value(edit, "alpha")
        wait_for(lambda: probe() == 3)
        u.SendMessageW(hwnd, 0x0111, 206, 0)
        wait_for(lambda: probe() == 2)
        u.SendMessageW(hwnd, 0x0111, 207, 0)
        wait_for(lambda: probe() == 1)
        u.SendMessageW(hwnd, 0x0111, 206, 0)
        wait_for(lambda: probe() == 2)
        u.SendMessageW(hwnd, 0x0111, 207, 0)
        wait_for(lambda: probe() == 3)
        u.SendMessageW(source_edit, 0x00B1, 0, 0)
        u.SendMessageW(source_edit, 0x0102, ord("Z"), 0)
        assert probe(2) == 0, "Editing discards stale index immediately"
        wait_for(lambda: probe() == 3)
        assert u.SendMessageW(source_edit, 0x00B0, 0, 0) == 0x10001, "Background refresh preserves caret"
        assert probe(1) == 0, "Background refresh does not activate a different match"
        u.SendMessageW(source_edit, 0x00C7, 0, 0)  # undo
        wait_for(lambda: probe() == 3)
        set_value(edit, "ångström")
        wait_for(lambda: probe() == 2)
        # Closing while a new query is pending must reject both discovery and indexing.
        set_value(edit, "Alpha")
        u.SendMessageW(hwnd, 0x0111, 204, 0)
        time.sleep(0.3)
        assert not probe(4) and not probe(2)
        # Source-only and preview-only both open the same source Find bar.
        u.SendMessageW(panel, 0x0111, 12, 0)  # Live off
        u.SendMessageW(hwnd, 0x0111, 101, 0)
        set_value(edit, "alpha")
        wait_for(lambda: probe() == 3)
        u.SendMessageW(hwnd, 0x0111, 204, 0)
        u.SendMessageW(panel, 0x0111, 2, 0)  # Preview
        time.sleep(0.6)
        u.SendMessageW(hwnd, 0x0111, 101, 0)
        wait_for(lambda: u.IsWindowVisible(source_edit))
        wait_for(lambda: probe() == 3)
        assert_search_focus()
        u.SendMessageW(panel, 0x0111, 51, 0)  # Replace
        assert not probe(4), "Replace dismisses the bar"
        print("PASS: editor options, revision cancellation, caret preservation, Unicode, modes, and Replace handoff")
    print(json.dumps({"first_match": first, "count": total, "next_match": paints[-1], "typing_clear": "passed"}))
    print(f"Results: {folder}")
finally:
    if hwnd:
        u.PostMessageW(hwnd, 0x0010, 0, 0)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
