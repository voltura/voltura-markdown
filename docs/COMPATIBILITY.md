# Compatibility

## Platform

Windows 10/11 x64. The app is Per-Monitor V2 DPI aware and uses Windows system graphics, shell, image, and networking libraries. No separate runtime is required.

Windows 11 at 225% scaling is locally validated. The maintainer reports clean Windows execution and default-app association checks completed on 2026-10-17. Automated scale-change checks cover 100%, 125%, 150%, 200%, 225%, and 300% in windowed and fullscreen modes, including negative window coordinates and returning to the original scale. These simulate application message handling; physical mixed-DPI monitor movement remains unverified. Separate first launches after reboot passed the 500 ms small-file and 2 s 10 MiB targets; Windows prefetching was not disabled. An elevated trace confirms 59.6 displayed updates/s on a 60 Hz display with the 10 MiB workload. Local self-signed signing/timestamping and isolated installer lifecycle tests are verified. See [performance and verification](../PERFORMANCE.md).

## Markdown and HTML

The pinned MD4C parser targets CommonMark 0.31.2, with tables, task lists, strikethrough, URL/email autolinks, footnotes, and GitHub alerts. Setext headings, indented/fenced code, references, nested lists, escapes, and hard breaks are supported.

The native renderer supports headings, inline formatting, quotes, lists, code, links, rules, tables, footnotes, and images. Raw HTML blocks use [Lexbor 3.0.0](https://github.com/lexbor/lexbor/releases/tag/v3.0.0) for [WHATWG HTML tree construction](https://html.spec.whatwg.org/multipage/parsing.html), including implied end tags, misnested formatting recovery, void elements, character references, comments, and table foster parenting. MD4C still determines CommonMark HTML block boundaries; inline HTML interleaved with Markdown uses the native formatting adapter.

The native presentation subset follows the [HTML rendering rules](https://html.spec.whatwg.org/multipage/rendering.html) for the supported semantics:

- Headings, paragraphs, divisions, semantic section/figure wrappers, lists, quotes, rules, preformatted text, expanded details/summary, links, and basic inline emphasis, code, keyboard keys, underline, deletion/insertion, small text, subscripts, and superscripts.
- Inherited left/center/right `align` hints on containers and table sections, rows, and cells. Images and captions follow their containing alignment.
- Table cells contain native blocks, including paragraphs, explicit line breaks, lists, pictures/images, captions, and recursively nested tables. Pixel/percentage cell `width` hints and top/middle/bottom `valign` are supported. `colspan` and `rowspan` follow the WHATWG table slot model, including row-group-scoped `rowspan="0"`, implied rows, standard integer recovery and span limits, footer ordering, and declared `col`/`colgroup` slots. Text offsets stay available for search and copying.
- HTML image dimensions and light/dark picture sources follow the image rules below. Comments do not appear in reading view.

This is standards-based parsing with a native presentation subset, not full browser rendering. Stylesheets, inline CSS, JavaScript, forms, frames, media playback, and browser table intrinsic-sizing algorithms are not implemented. Images occupy separate native blocks rather than arbitrary inline line boxes. Unsupported constructs remain readable where possible; scripts and CSS are never executed. Recursive table presentation is bounded to 64 cell levels. Table models use at most 1,000,000 logical rows and 65,536 logical columns; spans allocate sparse ranges rather than a dense slot matrix. See [the HTML document model](HTML-MODEL.md) for the implementation contract and verification.

Optional source editing includes insertion/table helpers, formatted live preview, Find/Replace, heading navigation, bounded source/code highlighting, recovery snapshots, managed images, validation, and explicit Save/Save As. HTML export and native printing (including Windows PDF printers) are available on demand. Highlighting uses common lexical tokens rather than a complete language service. Formatted-document editing, arbitrary HTML layouts, Mermaid, and mathematical typesetting are outside this version. Files containing null characters remain available for reading but cannot enter edit mode safely.

## Images and links

- Images: PNG, JPEG, first-frame GIF, and NanoSVG's static SVG subset.
- SVG badges support simple `<text>` labels and values using installed Windows fonts, inherited font/fill attributes, text anchors, `textLength`, and group/text transforms. Embedded base64 SVG vector icons and transparent link areas are supported. Glyphs become vector paths at the badge's intrinsic dimensions. SVG filter effects, complex text layouts, external resources, and animation remain unsupported.
- Local paths resolve relative to the document; percent-escaped paths work.
- HTML `<picture>` supports `<source>` with a single `srcset` URL (optionally `1x`), using the first matching light/dark `prefers-color-scheme` source for the reader's current theme. Sources without a media condition match both themes. Other media queries, responsive candidate lists, and unsupported image types fall back to `<img>`. The image keeps its alt text and explicit width/height. HTML export preserves these variants; printing uses the light image.
- HTML `<sub>` preserves caption text with smaller type and the font's subscript typography where available. Search and copy retain the text, and HTML export retains `<sub>`.
- Remote images use HTTPS and load asynchronously near the viewport. HTTP, UNC/network-share/device paths, external SVG resources, and animated images are unsupported.
- Images are limited to 32 MiB encoded data and constrained decoded dimensions; base decoding reduces the longest edge to at most 2048 pixels. SVGs can be rerasterized asynchronously up to 4096 pixels within the image-cache budget for sharper zoom/DPI rendering. Unknown dimensions can cause layout movement. Failures show alt text.
- Web/email links open through Windows. Internal headings/footnotes navigate within the document. Relative Markdown links open another reader window; cross-file fragments open at the destination's beginning.
- Executable and other file URL schemes are not launched.

## Text and document limits

UTF-8 is preferred. UTF-8 BOM and BOM-marked UTF-16 LE/BE are supported. Invalid UTF-8 without a UTF-16 BOM uses the current Windows ANSI code page. Malformed UTF-16 may show replacement glyphs.

Input limits: 1 GB of bytes and 500 million decoded UTF-16 units. Very long text/code lines use bounded visual segments; source copying preserves the original content. Long Markdown table cells use continuation rows. Exceptionally large rich HTML table bands (over 8,192 text units, 256 children, or 256 cells) display their native child blocks in source order, retaining text, images, links, and formatting while omitting table geometry. Ordinary tables wider than 256 columns display as readable text. A failed block layout falls back locally to wrapped text; a parser failure keeps the complete original text in reading view when memory permits. Source view is always an explicit choice.

Parsed structure and source memory grow with document size. The tested dense 100 MiB document peaks at about 1.3 GiB in the full viewer; [measurements](../PERFORMANCE.md) describe the tested workload. Layout/image caches are bounded. The viewer does not automatically reload external edits.
