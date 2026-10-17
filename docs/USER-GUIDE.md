# User guide

[Download options](../README.md#download) · [Supported formats](COMPATIBILITY.md)

## Open a document

Run `vmd.exe`, press **Ctrl+O**, or drop a Markdown file onto the window. Each launch opens a separate window.

```powershell
.\vmd.exe "C:\Documents\example.md"
```

The reader starts in a normal window without a document, and fullscreen when launched with a document (unless **Start fullscreen** is disabled). Use **F11** to switch window modes. **Escape** dismisses search or Help/About first. When launched with a file path (including from Explorer), it then closes directly, even in fullscreen. Without a file argument, it leaves fullscreen or closes a normal window. The fullscreen **X** and **Alt+F4** close immediately.

## Controls

| Action | Shortcut or icon |
| --- | --- |
| Help | F1 / three-dot menu → Help |
| Open | Ctrl+O / folder |
| Find | Ctrl+F / magnifier |
| Next / previous match | Enter or F3 / Shift+Enter or Shift+F3 |
| Formatted / source | Ctrl+Shift+V / code |
| Light / dark | Sun; three-dot menu → Use Windows theme |
| Fullscreen | F11 / corners |
| Zoom | Ctrl+wheel, Ctrl+plus/minus; Ctrl+0 resets |
| Copy selection / select all | Ctrl+C / Ctrl+A |
| Scroll | Wheel, trackpad, arrows, Page Up/Down, Space, Home/End, scrollbars |

**Up/Down** scroll vertically. **Left/Right** scroll the whole document horizontally when wide tables or code extend beyond the window.

Double-click selects a word; Shift+arrows extends selection. Source copying retains the original decoded text and line endings. Formatted copying produces displayed plain text. Checkboxes and text are read-only.

Zooming reveals **−, percentage, +** floating over the lower-right corner, inset from the scrollbars. The document keeps its full viewing area. Click the percentage to return to **100%**. The group fades away at 100% and follows fullscreen toolbar visibility.

Fullscreen controls hide after inactivity and return with pointer movement or keyboard focus. Change **Start fullscreen** in the three-dot menu. Theme, zoom, startup mode, and window placement are remembered.

## File actions

The three-dot menu provides:

- **Copy file** for pasting a copy into Explorer.
- **Copy file name** or **Copy full path** as text.
- **Open file directory** to show the document in Explorer.
- **Edit file with…** to choose an installed editor through Windows.

Reopen the document after editing it elsewhere; automatic reload is not included. Windows file/app dialogs follow the system appearance.

## Install, update, and defaults

The portable download is just `vmd.exe`. To update, close it and replace the file in the same folder.

Setup installs for your account under `%LOCALAPPDATA%\Programs\Voltura Markdown`. Run newer Setup to update; use Windows **Installed apps** to uninstall. Documents and preferences are retained.

To use the reader for `.md` files, select Setup's optional **Choose as default Markdown reader**, or choose **Register for .md files** in the app. Both open the app's page in Windows Default apps where supported; otherwise, find **Voltura Markdown** in Default apps. Confirm it for **.md** files. No administrator rights are required. Keep a portable copy at a stable path and register again if you move it. **Unregister application** removes its registration.

## Troubleshooting

| Problem | Try |
| --- | --- |
| Files open in another app | Register the reader, then select it for `.md` in Windows Default apps. |
| Missing image | Check the relative file path or HTTPS connection; failed images show alt text. |
| Unexpected characters | Save the document as UTF-8; see [encoding support](COMPATIBILITY.md). |
| Source appears instead of formatting | Keep a small, non-private reproduction for a bug report. |
| Reset preferences | Close the reader and rename `%LOCALAPPDATA%\Voltura\Markdown\preferences.ini`. |

Press **F1** for in-app Help. Back or Escape returns from Help/About to your reading position.
