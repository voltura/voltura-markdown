"""Run upstream CommonMark examples through MD4C and validate the native adapter."""

import json
import pathlib
import subprocess
import sys
from html.parser import HTMLParser

ROOT = pathlib.Path(__file__).resolve().parents[1]


class Canonical(HTMLParser):
    def __init__(self, s):
        super().__init__(convert_charrefs=True)
        self.parts = []
        self.in_pre = False
        self.feed(s)

    def handle_starttag(self, tag, attrs):
        if (
            not self.in_pre
            and tag in {"ul", "ol", "blockquote", "p", "pre", "hr"}
            and self.parts
            and self.parts[-1][0] == "text"
        ):
            self.parts[-1] = ("text", self.parts[-1][1].rstrip("\n"))
            if not self.parts[-1][1]:
                self.parts.pop()
        self.parts.append(("tag", tag, tuple(sorted(attrs))))
        if tag == "pre":
            self.in_pre = True

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)

    def handle_endtag(self, tag):
        self.parts.append(("end", tag))
        if tag == "pre":
            self.in_pre = False

    def handle_data(self, s):
        # HTML renderers may put newlines between block tags differently.
        # Preserve preformatted whitespace and inline spaces exactly.
        if not self.in_pre and not s.strip() and "\n" in s:
            return
        if self.parts and self.parts[-1][0] == "text":
            self.parts[-1] = ("text", self.parts[-1][1] + s)
        else:
            self.parts.append(("text", s))

    def handle_comment(self, s):
        self.parts.append(("comment", s))


exe = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/Release/reader_tests.exe"
examples = json.loads((ROOT / "tests/commonmark-0.31.2.json").read_text(encoding="utf-8"))
failed = []
for e in examples:
    p = subprocess.run([str(exe), "--html"], input=e["markdown"].encode(), capture_output=True)
    actual = p.stdout.decode("utf-8").replace("\r\n", "\n")
    if p.returncode or Canonical(actual).parts != Canonical(e["html"]).parts:
        failed.append(e["example"])
        print(f"FAIL {e['example']} {e['section']}: {p.stderr.decode('utf-8',errors='replace').strip()}")
print(
    f"CommonMark 0.31.2: {len(examples)-len(failed)}/{len(examples)} passed (HTML token normalization, native adapter bounds checked)."
)
sys.exit(bool(failed))
