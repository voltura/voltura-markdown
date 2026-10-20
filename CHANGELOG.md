# Changelog

## 1.3.5 - 2026-10-20

- Add a themed, draggable divider to resize side-by-side and stacked Live editor panes.
- Keep preview resizing smooth and clear stale text fragments from the margin beside the source scrollbar.
- Make Shift+Enter insert a Markdown source newline so code fences and line numbers remain aligned.

## 1.3.4 - 2026-10-20

- Add an integrated Find bar with match counts, previous/next controls, and match markers in the scrollbar.
- Add case-sensitive and whole-word search in the editor, with visible match highlights and Replace handoff.
- Improve split-editor wrapping, line numbers, and scrolling responsiveness.
- Refine insert dialogs and use the Explorer-style Open dialog.
- Add a context menu for document links.


## 1.3.3 � 2026-10-20

- Improve heading conversion and navigation, outline labels, list editing, and table commands while preserving surrounding Markdown.
- Preserve local links when saving elsewhere, including HTML links, encoded filenames, and query strings.
- Render and export image links, table images, nested image descriptions, HTML lists, and inline formatting more faithfully.
- Correct photo orientation and image decoding, and refresh missing or changed images when reloading a document.
- Improve syntax coloring for C#, C++, SQL, and PowerShell strings and comments.
- Preserve typography and image sizing in printed output, and recover older drafts safely without accepting damaged UTF-8 data.


## 1.3.2 — 2026-10-20

- Fixes duplicate heading links, list-heading anchors, explicit HTML heading identifiers, and HTML list numbering.
- Preserves heading text, nested quotes, task spacing, and enclosing quotes when editing.
- Protects fenced and indented code from table commands and retains missing final newlines.
- Corrects literal local-link validation and query-only links during export and Save As.
- Preserves separate tables, long code blocks, vertical cell alignment, and nested quotes in HTML exports.
- Rejects malformed numeric entities and respects UTF-8 byte-order marks when decoding damaged files.
- Adds regression coverage for all 20 fixes and native duplicate-heading navigation.

## 1.3.1 — 2026-10-20

- Preserves literal link labels and destinations and local links during Save As.
- Fixes heading conversion, outline labels, tab-separated list continuation, and overlapping whole-word replacements.
- Ignores malformed recovery filenames without hiding valid drafts.

## 1.3.0 — 2026-10-19

- Adds complete code-block copying, basic syntax coloring for common language labels, inline-code backgrounds, and colored links with destination hover feedback.
- Expands native HTML support with HTML5 tree construction, merged and nested tables, captions, image dimensions, themed pictures, small text, subscripts, and superscripts.
- Renders simple SVG badge text sharply across zoom/DPI changes and wraps adjacent Markdown badges together.
- Starts wide edit windows with side-by-side Live preview, adds View mode wording, and themes editor scrollbars, menus, dialogs, and file/print pickers.
- Keeps large rich cells on bounded readable blocks, preserves mixed header/data cells during HTML export and rules inside cells, and fixes preview heading navigation and scrolling after CRLF saves.
- Updates the README, built-in Help, user guide, product page, and compatibility limits to match the release.

## 1.2.0 — 2026-10-18

- Adds optional Markdown editing, new files, encoding-preserving Save/Save As, validation, and live preview.
- Adds formatting and image helpers, table editing, Find/Replace, heading navigation, smart lists, source highlighting, and synchronized preview scrolling.
- Adds background crash recovery, managed clipboard/drop images, HTML export, and native print/PDF.
- Adds opt-in stable updates with signature verification and restores unchanged open documents after manual updates.
- Keeps editor components off reader startup and uses bounded highlighting and incremental source capture for responsive editing.

## 1.1.8 — 2026-10-17

- Adds + and - keyboard shortcuts for zoom, including the numeric keypad, while preserving normal typing in Find.
- Updates Help with boxed zoom shortcuts and clarifies the existing Ctrl alternatives and reset shortcut.
- Updates zoom button tooltips and accessible names without adding startup or background work.

## 1.1.7 — 2026-10-17

- Adds Copy path to the existing default-app notice, copying the running executable location.
- Shows brief Copied or Copy failed feedback inside the button without expanding the notice or interrupting reading.
- Preserves the notice wording and the Open Default apps and Dismiss actions.

## 1.1.6 — 2026-10-17

- Keeps wide code backgrounds aligned with text and right-hand padding at high zoom.
- Aligns nested table backgrounds and borders, and images, with their indentation.
- Scales missing-image labels with document zoom and clips them inside their placeholder.
- Joins bounded sections of large code blocks with consistent measured widths and no internal background gaps.

## 1.1.5 — 2026-10-17

- Prevents malformed SVG style recursion and bounds stroke-dash expansion to avoid crashes and hangs.
- Handles incomplete SVG CSS declarations and malformed/singular transforms without invalid memory reads.
- Makes HTML entity decoding linear and cancellable, including long malformed input.
- Rejects invalid decoded local link filenames before launching another reader window.
- Cancels obsolete queued image requests on document changes and rejects stale results.
- Recovers from drawing-brush creation failure without retaining a partially initialized renderer.
- Allocates worker storage and result payloads before launching document/search threads and publishes image results without another allocation.
- Aligns product-page version labels and installer filenames with the selected release tag.

## 1.1.4 — 2026-10-17

- Prevents scrollbar flicker when changing zoom.
- Avoids horizontal scrolling into empty centering margins and uses the arrow cursor over scrollbars.
- Shows the current search match and total, with navigation hints when there are multiple hits and room to display them.
- Leaves two text lines of context above search matches where space permits, and fixes the search field's right edge.
- Excludes test options, verification routines, failure injection, and timing instrumentation from the production executable.
- Prevents the Windows beep when Enter or Shift+Enter navigates search results.

## 1.1.3 — 2026-10-17

- Detects when .md files point to Voltura Markdown at another location and offers to register the current copy and open Windows Default apps. Windows still confirms the final default choice.
- Defers association checks until after the first content is painted and runs shell work in the background, preserving fast software-first startup and responsive reading.
- Keeps uncertain associations quiet and supports dismissal, keyboard access, retry after registration failure, and closing while a check is stalled.

## 1.1.2 — 2026-10-17

- Uses markdown.voltura.se for the About link, executable metadata, installer website, and signing website URL.
- Moves the product site to its HTTPS custom domain while retaining the published domain on future updates.

## 1.1.1 — 2026-10-17

- Starts in a normal window when launched without a document.
- Opens the app's Default apps settings directly on supported Windows versions, with Windows confirmation for .md files.
- Clearer installer welcome and completion text.

## 1.1.0 — 2026-10-17

- Faster first-page display using software rendering while hardware rendering prepares in the background.
- Preserves reading position and selection when hardware rendering takes over.
- Continues displaying the document in software if hardware setup fails.

## 1.0.0 — 2026-10-17

- Reduced memory use and opening time for documents with many Markdown blocks.
- Correct initial control scaling and added DPI-transition regression coverage.

## 1.0.0 Preview 2 — 2026-10-17

- Timestamped, self-signed portable reader and Windows installer.
- Local certificate setup, automatic release signing, and signature verification.

## 1.0.0 Preview — 2026-10-16

- Portable, single-file Windows Markdown reader and optional per-user Setup.
- Formatted/source views, selection, copying, search, zoom, and smooth scrolling.
- Floating lower-right themed zoom controls with percentage, reset, a brief fade, and scrollbar clearance.
- Left/Right horizontal scrolling.
- CommonMark parsing with tables, task lists, footnotes, alerts, and local/HTTPS images.
- Common embedded HTML, themed keyboard keys, and readable local fallbacks within formatted view.
- Light/dark themes for the reader, menus, tooltips, scrollbars, Help, and About.
- Fullscreen startup, direct-close X, and direct Escape exit when launched with a file path.
- Copy file/name/path, show in Explorer, and choose an external editor.
- Optional default-reader setup through Windows Settings.
- Native document-and-lightning artwork and multi-resolution app icon.
- Background loading, bounded layout/image caches, Unicode boundary handling, and cancellable search.
- MIT-licensed application, documentation, and current artwork.

See [release notes](docs/releases/1.1.8.md) for downloads and [compatibility](docs/COMPATIBILITY.md) for limits.
