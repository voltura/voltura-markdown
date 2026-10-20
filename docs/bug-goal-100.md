# 100-bug goal

Baseline: `0292cb8d67acda25f5d9ccc6f4006f46cf901b9b`, primary checkout on `main`.
The prior 20 fixes in `bug-audit-2026-10-20.md` are excluded from this new goal.
Count only distinct reproduced defects, not individual assertions or speculative improvements.

## Verified fixes: 64 / 100

IDs remain stable when an audited case is excluded; the highest ID is not the count.

All counted regression cases failed before the fixes and passed afterwards.
Regression coverage: `tests/goal-bug-regressions.h`, `tests/goal-image-regressions.h`,
`tests/code-syntax.cpp`, and the encoded-heading click in `scripts/test-editing.py`.
Release build and all nine CTest suites passed on a private Windows desktop.

| ID | Reproduction / defect | Fix |
| --- | --- | --- |
| 001 | Enter after `1. [x] done` drops the task checkbox. | Continue ordered tasks with an unchecked checkbox. |
| 002 | Enter after `1.   item` reduces the marker's three spaces to one. | Preserve numbered-list marker spacing. |
| 003 | Save As from `source.md` to `copy.md` in the same directory leaves `?download=1` pointing to the copy. | Preserve the original query-only destination. |
| 004 | `other.md#part:one` is mistaken for a URL scheme during export and Save As. | Recognize schemes only before path, query, and fragment separators. |
| 005 | Table commands fail when the table immediately follows prose containing a pipe. | Find the header and delimiter pair within contiguous pipe-containing lines. |
| 006 | HTML export discards `<kbd>` semantics from keyboard keys. | Retain keyboard tags alongside monospace styling. |
| 007 | Export moves images and rules out of their blockquotes. | Preserve the quote depth around these non-text blocks. |
| 008 | Export turns each 8,192-character layout piece of a paragraph or heading into a separate element. | Join continuation pieces within the original element. |
| 009 | A long heading receives an identifier from only its last layout piece, and links target that piece. | Generate the identifier from the complete title and attach it to the first piece; preserve explicit identifiers there. |
| 010 | Shift+Tab on two spaces followed by a tab removes only the spaces, leaving the same visual indentation. | Remove one four-column indentation level, including mixed spaces and tabs. |
| 011 | Warnings for escaped link destinations select the wrong source position and length. | Locate and measure the raw source destination before decoding it. |
| 012 | A validated link to `#%64estination-1` does nothing when clicked. | Decode UTF-8 percent escapes before matching the heading identifier. |
| 013 | Local image URLs with query suffixes fail in the reader; query/fragment suffixes fail during printing/export decoding. | Remove URL suffixes before resolving local files, while retaining encoded filename characters. |
| 014 | An uppercase `HTTPS://` image URL is rejected. | Normalize the scheme without changing the case-sensitive path or query; apply this to redirects too. |
| 015 | SQL `/* ... */` comments are highlighted as ordinary code. | Recognize SQL block comments. |
| 016 | Backslashes in shell/SQL literal strings and backticks in PowerShell single-quoted strings consume the closing quote and swallow following code. | Apply each language's literal-string escape rules and retain doubled quotes where supported. |
| 017 | Heading links to HTML table-cell headings are reported missing and cannot navigate; the heading-link menu omits them. | Traverse semantic child blocks, including table cells and captions, for heading discovery. |
| 018 | `<p><br>Text</p>` loses its initial explicit blank line. | Create the text block when a leading HTML break is encountered. |
| 019 | `</a>` removes the underline inherited from an enclosing `<u>`. | Restore the prior link and underline state when an HTML link closes. |
| 020 | HTML list numbers such as `start=" 3 "` or `value="+7suffix"` fall back to the wrong number. | Accept the signed integer prefix, retaining the existing numeric bounds. |
| 021 | A tab-separated list fence such as `-\t` followed by three backticks is not recognized; smart Enter can alter its code. | Interpret structural-prefix tabs at four-column stops, including continuation indentation. |
| 022 | A closing fence followed by a vertical tab is accepted even though Markdown permits only blank characters there. | Restrict closing-fence suffixes to spaces/tabs and the CR in CRLF input. |
| 023 | A printed image with HTML dimensions 40×20 uses its intrinsic 160×80 size. | Use the shared image-sizing rules and constrain the requested size to the printable area. |
| 024 | Printing drops monospace inline code, superscripts, and small-text styling. | Apply the supported font, size, and script effects to each inline run. These related omissions count as one defect. |
| 025 | Replacing table content with print RTF discards inline bold, italic, and monospace styling. | Emit styled text runs when constructing table-cell RTF. |
| 026 | A code fence or heading directly after a table is rewritten as a table row when it contains a pipe. | Stop at these block boundaries, preserve preceding headings, and reject a caret outside the table. Hash-prefixed cells that are not headings remain editable. |
| 027 | Whole-word replacement replaces the base letter of a decomposed accented word, leaving its combining mark behind. | Treat nonspacing and vowel marks as word constituents, while preserving punctuation boundaries. |
| 029 | Markdown and HTML images with empty alt text lose their enclosing hyperlink, including in HTML export. | Retain a zero-length link run for the image independently of alt text. |
| 030 | HTML export converts explicit heading line breaks into plain newlines, which collapse under the heading's normal whitespace rules. | Emit `<br>` for explicit inline breaks, including styled text; retain literal newlines in code blocks. |
| 031 | Editing a table inside a list removes its leading indentation, moving the table outside the list item. | Preserve the header's indentation on every rewritten row; reparse the result to verify list membership. |
| 032 | C# verbatim strings use ordinary escape handling, so a trailing backslash swallows the closing quote and doubled quotes split the token. | Recognize `@`, `$@`, and `@$` string prefixes and apply verbatim quote rules. |
| 033 | Shell highlighting treats literal hashes in words, quoted-word suffixes, and escaped text as comments, hiding later code on the line. | Track shell word boundaries and unquoted escapes before recognizing a comment; retain real comments and line continuations. |
| 034 | Numeric highlighting splits signed decimal and hexadecimal exponents into separate tokens. | Include exponent signs followed by digits, distinguishing decimal `e` from hexadecimal `p` and preserving subtraction boundaries. |
| 035 | A Markdown heading containing an image receives an identifier from only the text before the image, breaking full-title links and duplicate numbering. | Generate its identifier after the complete heading, including image alt text and following text, and attach it to the first block. |
| 036 | Generated heading identifiers retain non-ASCII capitals, so lowercase links to accented or Greek headings fail and case variants evade duplicate numbering. | Apply invariant Unicode lowercasing to non-ASCII titles, retaining the ASCII fast path and explicit identifiers unchanged. |
| 037 | C++ raw-string quotes terminate highlighting early, allowing literal comment markers to hide subsequent code. | Recognize raw-string prefixes and bounded custom delimiters, then match the complete closing delimiter. |
| 038 | HTML code-block identifiers are dropped, so links to `<pre id="…">` have no target in the reader or exported HTML. | Retain the identifier on the first code block and emit it once when exporting, including chunked code. |
| 039 | Save As into another directory leaves relative HTML link and image destinations pointing into the new directory. | Rebase parsed HTML `href`, `src`, and supported single-resource `srcset` values through shared destination logic. Map multiline HTML back to source spans to preserve list and blockquote prefixes; leave comments, raw text, and code examples intact. |
| 040 | PowerShell here-strings use ordinary quote termination, misclassifying literal comments and following code. | Recognize both here-string forms and their line-start terminators, including CRLF; confirm the reproduction with PowerShell's native parser. |
| 041 | The Normal text command inserts `Heading` into an empty document or blank line. | Suppress the heading placeholder for level zero while retaining it for actual heading insertion. |
| 042 | Heading commands insert markers outside blockquotes, while Normal text fails to remove a quoted heading marker. | Apply heading edits after the quote prefix, preserving nested quote depth and respecting tab-expanded code indentation. |
| 043 | Shift+Tab with no selection selects the whole outdented line, so subsequent typing replaces the line. | Preserve a collapsed caret at its adjusted position; native regression checks cover typing afterwards and clamping before indentation. |
| 044 | Enter after a spaced thematic break such as `* * *` invents a bullet on the next line. | Recognize thematic breaks before list continuation, retaining indentation or blockquote prefixes without a list marker. |
| 045 | Enter on an empty list item keeps creating list items instead of exiting because Rich Edit includes a paragraph terminator in the retrieved line. | Remove the native trailing carriage return before interpreting the line; native checks cover the final line, a following paragraph, and quoted lists. |
| 046 | HTML `<ol reversed>` displays ascending numbers and starts at one when no start is specified. | Derive the default start from direct list-item children in the normalized HTML tree, retain the counter direction, and honor explicit starts and item-value resets. |
| 047 | An apostrophe digit separator in C/C++ numbers such as `1'000` starts a string token, incorrectly coloring the following code. | Consume separators between numeric digits, including hexadecimal, binary, fractional, and exponent forms; retain ordinary character-literal boundaries. |
| 048 | A valid PNG containing `<svg` in text metadata is misclassified as SVG and fails to load or export. | Require an XML-like opening before selecting the SVG decoder, retaining XML declarations, comments, and UTF-8 BOMs; verify a PNG with a valid metadata chunk through both decoding paths. |
| 049 | JPEG and TIFF orientation metadata is ignored, displaying rotated or mirrored photos incorrectly. | Apply the EXIF transform after bounded scaling; verify all eight orientations against dimensions and expected corner pixels, reader/export parity, JPEG APP1 metadata, and invalid values. |
| 050 | The editor outline joins words separated by inline HTML line breaks, showing `FirstSecond` for `First<br>Second`. | Retain line-break spacing in heading labels, including uppercase and attributed tags, while preserving literal code and formatting-only tags. |
| 051 | Headings containing line breaks receive identifiers containing literal newlines, breaking normalized fragment links and emitting invalid whitespace-bearing HTML IDs. | Normalize ASCII whitespace to hyphens before duplicate numbering; retain explicit author identifiers. |
| 052 | Table commands miscount pipes inside HTML attributes, comments, link destinations, and titles, refusing valid tables or addressing the wrong column. | Identify protected inline spans with the Markdown parser before splitting cells; preserve Unicode offsets, quoted delimiters, code spans, autolinks, and image titles. |
| 053 | Markdown images end their table row, leaving images and neighboring text outside their cells and breaking exported table structure. | Promote affected cells to bounded rich content, preserve header and row identity, retain neighboring cells during large-row fallback, and leave ordinary tables on the plain-cell path. |
| 054 | Nested image labels create extra display images, interpret alt HTML as markup, and validate or rebase destinations that are only alt text. | Track the alt-text context across nested spans, preserve literal HTML and outer links, normalize alt line breaks, and apply the same semantics to outlines, validation, and Save As. Save As regressions cover nested labels, literal HTML, linked images, and returning to real links after alt text. |
| 055 | Normalized HTML blocks reinterpret literal tags inside raw-text elements as real images and headings. | Escape raw-text DOM contents before serialization to the native adapter; preserve literal entities and resume normal markup after each element. |
| 056 | Table commands stop at valid body rows with no pipe separators, leaving column changes applied to only part of the rendered table and disabling row commands there. | Include pipe-free body rows using the Markdown parser to check continuation, pad missing cells, and preserve surrounding block boundaries. |
| 057 | C# raw strings terminate highlighting at embedded quotes, so literal comment markers can hide following code. | Match the complete opening quote count before ending a raw string; cover multiline and interpolated forms, unfinished input, and ordinary verbatim-string boundaries. |
| 058 | Normal text leaves Setext underlines in place, and changing heading level leaves the old delimiter behind. | Expose heading source bounds and Setext identity from the vendored parser; include the complete heading in the explicit formatting edit and remove its underline. Cover quoted and multiline text, empty/reference images, Unicode offsets, selections, separate rules, and native single-step Undo. |
| 059 | The outline omits headings without text callbacks and navigates image-led multiline headings to their later text line. | Use parser heading source bounds for outline positions independently of title text; show Untitled heading for empty titles. Regression coverage includes Unicode offsets, nested and reference-image headings, and native caret navigation. |
| 060 | Tabs in heading titles split outline rows into a label and a false keyboard-shortcut badge. | Normalize tabs to spaces in outline titles after entity decoding, covering ordinary text, inline code, and numeric entities. Native menu sizing checks pass in both themes; all nine suites pass. |
| 061 | Normal text before a following `---` rule creates a new Setext heading, so the heading remains and the rule disappears. | Check the resulting adjacent-line syntax and insert a blank separator only when it would become Setext. Preserve quote prefixes; cover single-caret and selected ATX/Setext conversions, unambiguous rules, and native editing. All nine suites pass. |
| 062 | A recovery snapshot is hidden when its former process ID has been reused by a live process. | Compare the snapshot write time with process creation before classifying it as a live-session draft. Retain conservative live-process handling when timing queries are unavailable. A dated fixture reproduces reuse and verifies recovery content plus exclusion of current-lifetime drafts; all nine suites pass. |
| 063 | Recovery silently substitutes replacement characters for damaged UTF-8 in draft text or its original path. | Validate recovery encoding before decoding, skip malformed paths during discovery, and report corrupt snapshots without deleting them. Test truncated, surrogate, and overlong sequences alongside valid Unicode and intentional replacement characters; all nine suites pass. |
| 064 | Inserted link labels containing blank lines or block markers break the generated link into separate Markdown blocks; Link to heading can supply such labels from explicit heading breaks. | Encode normalized label newlines as character entities. Preserve multiline text, CRLF, punctuation, literal entities, and image alt text within a single link/image; all nine suites pass. |
| 065 | Failed images remain cached across document generations, and explicit reload retains stale successful images. | Retry failed images on generation change and clear completed images on document reload; retain successful images during live preview. The retry regression failed before the fix; retry, refresh, snapshot preservation, and all nine suites pass. |

Case 057 follows Microsoft's [C# raw-string delimiter rules](https://learn.microsoft.com/en-us/dotnet/csharp/fundamentals/strings/raw-string-literals).
Its regression failed before the fix; all nine suites passed afterwards.

After 058, all nine suites and 652 CommonMark cases passed. Private-desktop
benchmark `build/bench/bug-goal-100-setext.json` measured a 10 MiB median load/parse
total of 512.187 ms and first content at 565.8015 ms across two runs. This is a
current-build measurement, not a controlled before/after or physical-display claim.

## Release 1.3.3 verification

Final versioned build passed all nine CTest suites, 652 CommonMark examples,
formatting, native editing, startup, windowed/fullscreen exit paths, and the
isolated installer lifecycle test. Installer tests covered locked-file recovery,
upgrade, relocation, uninstall ownership, binary identity, and preserved user data.
GUI checks ran on private desktops. Installer artwork and layout are unchanged.

## Broader verification

- CommonMark: 652/652 examples passed.
- After 054, all nine suites and 652 CommonMark cases passed. The expanded native
  table-image fixture verified the nested-alt image and its outer hyperlink
  (`build/display-test-aa37a0385de546ec8220e7071c8f1c95`, private desktop).
- After 053, native table-image rendering and link hit-testing passed with
  `tests/empty-alt-images/markdown-table-images.md` on a private desktop
  (`build/display-test-f479d0f81b0f49f2824e12a47a6418b8`).
  Release and all nine suites passed, followed by 652/652 CommonMark cases.
  `build/bench/bug-goal-100-table-images.json` recorded 10 MiB median total
  516.19 ms and first content 564.64 ms; this is an uncontrolled current-build measurement.
- After 046, the Release build and all nine suites passed again, followed by
  652/652 CommonMark cases. The two-run benchmark in
  `build/bench/bug-goal-100-reversed-lists.json` measured a 10 MiB median total
  of 519.35 ms and first content of 564.93 ms. This is an uncontrolled current-build
  measurement, not a before/after comparison.
- Final Release rebuild: 9/9 CTest suites passed; formatting and whitespace checks passed.
- Native editing suite passed, including the reproduced encoded-heading click into a table cell,
  source/preview synchronization, both themes, save conflicts, and recovery.
- Startup checks passed for empty and document launches.
- Search checks passed: first-match paint 9.26 ms, next-match paint 6.54 ms.
- Two-run benchmark completed for 1 KiB, 1 MiB, and 10 MiB documents, plus long
  paragraph/code/table cases. Results: `build/bench/bug-goal-100.json`.
  The 10 MiB median load/parse/check time was 526.54 ms; first content was 566.82 ms.
  These are current-build measurements, not a controlled before/after comparison.
- After 035, the same two-run benchmark completed again:
  `build/bench/bug-goal-100-heading.json`. The 10 MiB median load/parse/check time
  was 528.68 ms and first content was 558.71 ms. These remain uncontrolled
  measurements; the earlier run above is historical, not a performance guarantee.
- GUI checks ran on private Windows desktops; no physical-display proof is claimed.
- Image regressions use real local decoders and a fake remote loader. They verify
  URL dispatch and preservation, not live HTTPS availability. Existing tests still
  verify rejection of UNC and Windows device paths.
- Fence fixes pass both direct scanner tests and native smart-Enter checks in
  `Editor::verify`, compiled only with `VMD_TESTING`.
- Empty-alt image links pass the native hit-testing, wrapping, and DPI checks using
  `scripts/test-display.ps1 -Document tests/empty-alt-images/inline-images.md`.
  The fixture uses local SVGs and does not fetch remote images.
- Printing was reproduced through Microsoft Print to PDF. `scripts/test-print-format.py`
  failed for 023–025 before the fixes and passed afterwards. It checks actual PDF
  image geometry, font identity, size, and script positioning using optional
  `pdfplumber`; page-one renders were visually inspected.
  Latest output: `build/editor-export-cedbd9c4649745e8bf6727448d007cc1/document.pdf`.

A suspected recovery-discovery defect (a directory ending in `.draft`) did not
reproduce and is excluded from the count. The goal remains incomplete.
The suspected mixed-header print defect also did not reproduce and is excluded;
its check remains as additional coverage for the table-formatting fix.
The earlier simple multiline link label passed without a fix and was excluded.
Blank lines and block-marker lines subsequently reproduced the distinct failure
recorded in 064.
Encoded NUL image URLs were already rejected; their regression remains uncounted.
Case 028 is excluded after a reachability audit: `Editor::Editor` rejects documents
containing NUL before creating the editing UI, and reload/recovery paths also
reject NUL input. The outline helper's replacement-character handling and its
regression test remain as defensive coverage, not a counted user-visible fix.

The user ended the audit early at 64 verified fixes and authorized release 1.3.3.
The 100-bug goal is paused; it is not complete.
