# User guide

[Download options](../README.md#download) · [Supported formats](COMPATIBILITY.md)

## Open a document

Run `vmd.exe`, press **Ctrl+O**, or drop a Markdown file onto the window. Each launch opens a separate window.

```powershell
.\vmd.exe "C:\Documents\example.md"
```

The reader starts in a normal window without a document, and fullscreen when launched with a document (unless **Start fullscreen** is disabled). Use **F11** to switch window modes. **Escape** dismisses search or Help/About first. When launched with a file path (including from Explorer), it then closes directly, even in fullscreen. Without a file argument, it leaves fullscreen or closes a normal window. In reading mode, the fullscreen **X** and **Alt+F4** close immediately; modified drafts first ask whether to save.

## Controls

Code blocks have a copy icon in their upper-right corner. Hover to see **Copy code to clipboard**; click to copy the complete block, with indentation and line breaks preserved. **Copied!** confirms success.

Fenced blocks with a language label use basic syntax coloring in both themes: PowerShell (`powershell`, `pwsh`, `ps1`), TypeScript/JavaScript (`typescript`, `ts`, `tsx`, `javascript`, `js`, `jsx`), C# (`csharp`, `cs`, `c#`), C/C++, Python, JSON/JSONC, SQL, and shell/Bash. Unlabeled and unknown languages remain plain text.

Inline code has a subtle background. Colored links show their destination on hover; internal heading links navigate within the document and keep synchronized Live source aligned.

Supported HTML includes tables with merged cells, captions, nested tables and images; explicit image dimensions, light/dark pictures, small text, subscripts, superscripts, and keyboard keys. Simple SVG badges retain sharp text as zoom changes, and adjacent Markdown badges wrap together. Exceptionally large rich table bands show readable child blocks instead of table geometry. See [supported formats and limits](COMPATIBILITY.md) for the native presentation subset.

| Action | Shortcut or icon |
| --- | --- |
| Help | F1 / three-dot menu → Help |
| Open | Ctrl+O / folder |
| Find | Ctrl+F / magnifier |
| Next / previous match | Enter or F3 / Shift+Enter or Shift+F3 |
| Formatted / source | Ctrl+Shift+V / code |
| Light / dark | Sun; three-dot menu → Use Windows theme |
| Fullscreen | F11 / corners |
| Zoom | + / -, Ctrl+wheel, Ctrl+plus/minus; Ctrl+0 resets |
| Copy selection / select all | Ctrl+C / Ctrl+A |
| Scroll | Wheel, trackpad, arrows, Page Up/Down, Space, Home/End, scrollbars |

**Up/Down** scroll vertically. **Left/Right** scroll the whole document horizontally when wide tables or code extend beyond the window.

Horizontal scrolling appears only when content extends beyond the viewport. Scrollbar tracks and dragging use the arrow cursor; document text uses the text-selection cursor.

Double-click selects a word; Shift+arrows extends selection. Source copying retains the original decoded text and line endings. Formatted copying produces displayed plain text. Checkboxes and text are read-only.

Press **+** to zoom in or **-** to zoom out, including on the numeric keypad. While typing in Find, these keys enter search text.

Zooming reveals **−, percentage, +** floating over the lower-right corner, inset from the scrollbars. The document keeps its full viewing area, and scrollbar sizes update with the zoomed content without a second adjustment from background layout. Click the percentage to return to **100%**. The group fades away at 100% and follows fullscreen toolbar visibility.

Fullscreen controls hide after inactivity and return with pointer movement or keyboard focus. Change **Start fullscreen** in the three-dot menu. Theme, zoom, startup mode, and window placement are remembered.

## Find text

Press **Ctrl+F** and type a phrase. The first match appears without waiting for the total count. The count fills in afterward, for example **3 of 12**; **0 of 0** means no matches. Search is case-insensitive and counts overlapping occurrences consistently in both directions.

Use **Enter** or **F3** for the next hit, and **Shift+Enter** or **Shift+F3** for the previous hit. Navigation wraps at either end. A shortcut hint appears below the field for multiple hits when the window has enough room. Matches keep about two text lines above them for context when preceding content and viewport space allow. **Escape** closes search.

## Edit and create Markdown

Choose **Edit Markdown** in the three-dot menu or press **Ctrl+E**. **Ctrl+N** creates an untitled draft. If a full file path supplied at startup does not exist, the app asks whether to create a draft at that location. No file is written until Save.

The edit toolbar provides Heading (levels 1–6 or Normal text), Bold (**Ctrl+B**), Italic (**Ctrl+I**), Underline (**Ctrl+U**), Link (**Ctrl+K**), Code, List, Image, and Insert. Insert includes tables, images, bulleted/numbered/task lists, quotes, code blocks, horizontal rules, strikethrough, and inline code. Table asks for body rows and columns and adds a header row. Link and image dialogs accept text and a destination; Image also offers a file picker and a Placeholder button for `![Image description](<path/to/image.png>)`. Image destinations can be local file paths or HTTPS URLs. Helpers apply to the selection or insert selected placeholder text, and each helper can be undone once with **Ctrl+Z**. **Ctrl+Y** redoes it. **More…** holds overflow actions in narrow windows. Underline inserts `<u>…</u>`, which the reader supports; Markdown underscores retain their usual emphasis meaning.

**Live** keeps source and formatted preview visible together and refreshes after a typing pause. Edit sessions opened in windows at least 1000 logical pixels wide start with source on the left and live preview on the right. Smaller windows start with the source/preview toggle; enabling Live manually stacks the panes. Drag the dotted divider to adjust pane widths side by side or pane heights when stacked. Live can be turned off at any size. **Code** wraps the selection in a fenced code block or inserts a selected placeholder.

**Preview** or **Ctrl+Shift+V** shows the formatted draft; **Source** returns to editing with caret, scroll, and undo history retained. **Ctrl+F** opens Find; **Ctrl+H** opens Find/Replace, with Match case, Whole word, Replace, and Replace All. Replace All runs in the background and creates one undo step. A changed draft rejects an older replacement result.

Enter continues list numbers, unchecked tasks, quotes, and indentation; an empty item ends its marker. Shift+Enter inserts a source newline without continuing a list marker. Tab inserts a tab at the caret or indents selected lines; Shift+Tab removes indentation. **Ctrl+Tab** moves to the edit controls, where Tab/Shift+Tab move between controls. Insert → **Document outline** (**Ctrl+Shift+O**) jumps to a heading. **Link to heading** inserts an internal heading link. Ctrl+click a live-preview heading to reach its source. Live-preview scrolling is synchronized proportionally in both directions; Insert can turn synchronization off.

Insert → **Edit table at caret** adds/removes rows and columns or changes a column's alignment. Place the caret in the relevant cell first; header rows and the last column cannot be deleted. **Source and code highlighting** colors the visible source after a pause, with common code tokens inside fenced blocks. It can be switched off in Insert.

Save a document before adding managed images. Drop an image file into an editing window, use **Copy image into assets folder**, or paste a clipboard image with Ctrl+V / **Paste image from clipboard**. Files are copied into `<document-name>.assets` beside the document; names never overwrite an existing image. PNG/JPEG/GIF/SVG/BMP/WebP file types are accepted; preview depends on Windows image codecs and the supported SVG subset. Clipboard images are encoded as PNG in the background. Markdown-file drops still open documents normally. Save As rebases local image/link destinations when changing folders; it leaves the existing asset files in place.

Insert → **Export HTML** writes a formatted document with local links and images resolved against the Markdown file. **Print / PDF** opens the Windows printer chooser, lays out tables, and embeds available images. Select Microsoft Print to PDF to choose a PDF filename. Exporting and printing do not save or alter the draft.

Validation runs after a short typing pause and before saving. The warning list identifies unclosed code fences, missing local link/image targets, and same-document heading links without targets. Select a warning to jump to its source. Relative-file checks wait for a save location in untitled documents. Remote URLs and network-share targets are not checked. Large warning lists show the first 500 issues plus an omitted count. Warnings allow saving; they are suggestions, not a claim that Markdown is invalid.

**Ctrl+S** saves; **Ctrl+Shift+S** opens Save As. Existing encodings, BOMs, and unchanged line endings are retained; new files default to UTF-8 without BOM and Windows line endings. If the original encoding cannot represent an edit, the app asks before converting to UTF-8. Changes made by another app produce an overwrite/Save As/cancel choice. Save failures retain the draft.

An asterisk marks unsaved changes. **View mode** switches from editing to reading. It prompts to save, discard, or cancel when necessary, as do Escape, opening another document, New, and closing. In preview, Escape returns to source first. Update restarts are deferred while a modified draft is open. Reading remains read-only, and editor components load only on entering edit mode.

Local crash-recovery snapshots are written in the background while editing, including during continuous typing, under `%LOCALAPPDATA%\Voltura\Markdown\Recovery`. They never save over the original file. After a crash, open a new draft and choose Insert → **Recover draft**. Recovery opens an unsaved document for Save As. Drafts in a running process are excluded; successful saves, undo back to the clean baseline, explicit discard, and normal closing clear obsolete snapshots. An abrupt crash can still lose changes since the last completed checkpoint. The original file always requires explicit Save.

### Editing shortcuts

Insert → **Cancel pending action** stops an active background image, replacement, or export operation. Crash snapshots support up to 64 million UTF-16 code units and 128 MiB of encoded snapshot data; a warning reports a checkpoint that cannot be written. Source highlighting processes a bounded visible region after a pause. Table helpers are bounded to 1 MiB, 10,000 rows, and 256 columns to keep interactive commands responsive.

| Action | Shortcut |
| --- | --- |
| Heading 1–6 / normal text | Ctrl+Shift+1–6 / Ctrl+Shift+0 |
| Bulleted / numbered / task list | Ctrl+Shift+8 / Ctrl+Shift+7 / Ctrl+Shift+9 |
| Code block / inline code | Ctrl+Shift+C / Ctrl+Shift+K |
| Quote / strikethrough | Ctrl+Shift+Q / Ctrl+Shift+X |
| Image / live preview | Ctrl+Shift+I / Ctrl+Shift+L |
| Undo / redo | Ctrl+Z / Ctrl+Y; also available in Insert |
| Find / Replace / outline | Ctrl+F / Ctrl+H / Ctrl+Shift+O |

## File actions

The three-dot menu provides:

- **Copy file** for pasting a copy into Explorer.
- **Copy file name** or **Copy full path** as text.
- **Open file directory** to show the document in Explorer.
- **Edit file with…** to choose an installed editor through Windows.

Edit mode checks for external file changes in the background. Insert → **Reload from disk** reloads explicitly and asks before discarding a modified draft. Saving still checks the exact disk contents for conflicts. Reading mode requires reopening the document. Menus, dialogs, and file/print pickers follow the app's light/dark theme.

## Install, update, and defaults

The portable download is just `vmd.exe`. To update, close it and replace the file in the same folder.

Setup installs for your account under `%LOCALAPPDATA%\Programs\Voltura Markdown`. Run newer Setup to update; use Windows **Installed apps** to uninstall. Documents and preferences are retained.

The three-dot menu's **Automatic updates** option is off by default. Enable it to download newer stable releases after publisher-signature and checksum verification, then install on close. Manual updates restore unchanged open documents and reading positions; modified drafts defer update restarts.

To use the reader for `.md` files, select Setup's optional **Choose as default Markdown reader**, or choose **Register for .md files** in the app. Both open the app's page in Windows Default apps where supported; otherwise, find **Voltura Markdown** in Default apps. Confirm it for **.md** files. No administrator rights are required. Keep a portable copy at a stable path and register again if you move it. **Unregister application** removes its registration.

## Troubleshooting

If `.md` files point to Voltura Markdown at another location, a notice appears after the reader is ready. **Open Default apps** registers this copy's location and opens Windows Settings for you to confirm your choice. **Dismiss** hides the notice until the next launch. If moving the executable prevents files from opening, launch the app directly to see the notice.

| Problem | Try |
| --- | --- |
| Files open in another app | Register the reader, then select it for `.md` in Windows Default apps. |
| Missing image | Check the relative file path or HTTPS connection; failed images show alt text. |
| Unexpected characters | Save the document as UTF-8; see [encoding support](COMPATIBILITY.md). |
| Source appears instead of formatting | Keep a small, non-private reproduction for a bug report. |
| Reset preferences | Close the reader and rename `%LOCALAPPDATA%\Voltura\Markdown\preferences.ini`. |

Press **F1** for in-app Help. Back or Escape returns from Help/About to your reading position.
