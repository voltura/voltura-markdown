# Compatibility

## Platform

Windows 10/11 x64. The app is Per-Monitor V2 DPI aware and uses Windows system graphics, shell, image, and networking libraries. No separate runtime is required.

Current validation: Windows 11 at 225% scaling. Clean Windows 10/11, physical mixed-DPI movement, actual default-app selection in an isolated profile, publicly trusted signing, cold-cache startup, and display-frame delivery remain unverified. Local self-signed Authenticode signing and timestamping are verified. Installer lifecycle checks cover install, upgrade, relocation, removal, and retained documents/preferences. See [performance and verification](../PERFORMANCE.md).

## Markdown and HTML

The pinned MD4C parser targets CommonMark 0.31.2, with tables, task lists, strikethrough, URL/email autolinks, footnotes, and GitHub alerts. Setext headings, indented/fenced code, references, nested lists, escapes, and hard breaks are supported.

The native renderer supports headings, inline formatting, quotes, lists, code, links, rules, tables, footnotes, and images. Safe HTML includes basic inline formatting and themed `<kbd>` keyboard keys, headings, paragraphs/divisions, line breaks, anchors, images, tables, nested lists, quotes, rules, preformatted text, and expanded details/summary. Harmless structural wrappers preserve their content; unsupported constructs remain readable in the formatted document. Scripts and CSS are not executed.

Editing, printing, syntax highlighting, arbitrary HTML layouts, Mermaid, and mathematical typesetting are outside this version.

## Images and links

- Images: PNG, JPEG, first-frame GIF, and NanoSVG's static SVG subset.
- Local paths resolve relative to the document; percent-escaped paths work.
- Remote images use HTTPS and load asynchronously near the viewport. HTTP, UNC/network-share/device paths, external SVG resources, and animated images are unsupported.
- Images are limited to 32 MiB encoded data and constrained decoded dimensions; decoding reduces the longest edge to at most 2048 pixels. Unknown dimensions can cause layout movement. Failures show alt text.
- Web/email links open through Windows. Internal headings/footnotes navigate within the document. Relative Markdown links open another reader window; cross-file fragments open at the destination's beginning.
- Executable and other file URL schemes are not launched.

## Text and document limits

UTF-8 is preferred. UTF-8 BOM and BOM-marked UTF-16 LE/BE are supported. Invalid UTF-8 without a UTF-16 BOM uses the current Windows ANSI code page. Malformed UTF-16 may show replacement glyphs.

Input limits: 1 GB of bytes and 500 million decoded UTF-16 units. Very long text/code lines use bounded visual segments; source copying preserves the original content. Long table cells use continuation rows. Tables wider than 256 columns display as readable text within the formatted document. A failed block layout falls back locally to wrapped text; a parser failure keeps the complete original text in reading view when memory permits. Source view is always an explicit choice.

Parsed structure and source memory grow with document size. Dense 100 MiB stress documents can require several GiB; [measurements](../PERFORMANCE.md) describe the tested workload. Layout/image caches are bounded. The viewer does not automatically reload external edits.
