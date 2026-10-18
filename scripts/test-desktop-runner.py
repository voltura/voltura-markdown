"""Check desktop isolation, argument/exit propagation, and owned process-tree cleanup."""

import ctypes as c
from ctypes import wintypes as w
import json
import pathlib
import subprocess
import sys
import tempfile
import time
import unittest

from test_desktop import api, check, desktop_name

ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = ROOT / "scripts/test_desktop.py"


def command(code, *arguments):
    return [sys.executable, str(RUNNER), "--", sys.executable, "-c", code, *arguments]


def input_desktop():
    user = c.WinDLL("user32", use_last_error=True)
    handle = check(api(user, "OpenInputDesktop", w.HANDLE, w.DWORD, w.BOOL, w.DWORD)(0, False, 1))
    try:
        name = c.create_unicode_buffer(256)
        check(
            api(
                user,
                "GetUserObjectInformationW",
                w.BOOL,
                w.HANDLE,
                c.c_int,
                c.c_void_p,
                w.DWORD,
                c.POINTER(w.DWORD),
            )(handle, 2, name, c.sizeof(name), None)
        )
        return name.value
    finally:
        api(user, "CloseDesktop", w.BOOL, w.HANDLE)(handle)


class DesktopTests(unittest.TestCase):
    def test_desktop_arguments_output_and_exit(self):
        active = input_desktop()
        code = (
            "import sys, json; sys.path.insert(0, sys.argv[1]); "
            "from test_desktop import desktop_name; "
            "print(json.dumps([desktop_name(), sys.argv[2:]])); "
            "print('stderr preserved', file=sys.stderr); sys.exit(17)"
        )
        arguments = ["spaces and åäö", 'embedded "quote"', "trailing slash\\", ""]
        result = subprocess.run(
            command(code, str(ROOT / "scripts"), *arguments), capture_output=True, text=True, timeout=20
        )
        self.assertEqual(result.returncode, 17, result.stderr)
        name, received = json.loads(result.stdout.splitlines()[-1])
        self.assertTrue(name.startswith("VolturaTests-"))
        self.assertNotEqual(name, desktop_name())
        self.assertEqual(received, arguments)
        self.assertIn("stderr preserved", result.stderr)
        self.assertEqual(input_desktop(), active)

    def assert_stopped(self, pid):
        kernel = c.WinDLL("kernel32", use_last_error=True)
        handle = api(kernel, "OpenProcess", w.HANDLE, w.DWORD, w.BOOL, w.DWORD)(0x100000, False, pid)
        if handle:
            try:
                self.assertEqual(
                    api(kernel, "WaitForSingleObject", w.DWORD, w.HANDLE, w.DWORD)(handle, 5000),
                    0,
                    f"Owned descendant {pid} survived",
                )
            finally:
                api(kernel, "CloseHandle", w.BOOL, w.HANDLE)(handle)

    def check_tree_cleanup(self, abort):
        with tempfile.TemporaryDirectory(prefix="voltura-desktop-å-") as directory:
            marker = pathlib.Path(directory) / "child pid.txt"
            code = (
                "import subprocess, sys, pathlib, time; "
                "p = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)']); "
                "pathlib.Path(sys.argv[1]).write_text(str(p.pid)); "
                + ("time.sleep(60)" if abort else "sys.exit(0)")
            )
            with subprocess.Popen(
                command(code, str(marker)), stdout=subprocess.DEVNULL, stderr=subprocess.PIPE
            ) as process:
                try:
                    deadline = time.monotonic() + 10
                    while not marker.exists() and time.monotonic() < deadline:
                        time.sleep(0.02)
                    self.assertTrue(marker.exists(), "Child did not start")
                    pid = int(marker.read_text())
                    if abort:
                        process.terminate()
                    process.wait(timeout=10)
                    self.assert_stopped(pid)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=5)

    def test_descendant_cleanup_on_completion(self):
        self.check_tree_cleanup(False)

    def test_descendant_cleanup_on_runner_termination(self):
        self.check_tree_cleanup(True)

    def test_launch_failure_is_reported(self):
        result = subprocess.run(
            [sys.executable, str(RUNNER), "--", "voltura-missing-test-command.exe"],
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("FileNotFoundError", result.stderr)


if __name__ == "__main__":
    unittest.main()
