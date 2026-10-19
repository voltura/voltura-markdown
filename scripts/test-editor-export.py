"""Verify actual multipage PDF output with a table and embedded SVG image via a Windows PDF printer."""

import pathlib
import re
import subprocess
import time
import uuid

from test_desktop import isolate

isolate()
root = pathlib.Path(__file__).resolve().parents[1]
folder = root / "build" / ("editor-export-" + uuid.uuid4().hex)
folder.mkdir()
output = folder / "document.pdf"
subprocess.run(
    [str(root / "build/Release/edit_tests.exe"), "--print-pdf", str(output)], check=True, timeout=45
)
deadline = time.monotonic() + 15
while time.monotonic() < deadline:
    if output.exists() and output.read_bytes().rstrip().endswith(b"%%EOF"):
        break
    time.sleep(0.1)
data = output.read_bytes()
assert data.startswith(b"%PDF-") and data.rstrip().endswith(b"%%EOF"), "Complete PDF required"
pages = len(re.findall(rb"/Type\s*/Page\b", data))
images = len(re.findall(rb"/Subtype\s*/Image\b", data))
assert pages >= 2, "Printing must paginate"
assert images >= 5, "Standalone, ordinary, colspan, rowspan, and nested cell images must all print"
print(f"PASS: PDF has {pages} pages and {images} embedded image(s). Artifacts: {folder}")
