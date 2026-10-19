# Native HTML document model

VMD is a native Markdown document reader/editor. It uses HTML standards to define
its supported document semantics; it does not implement a browser runtime or CSS
layout engine. The input Markdown is preserved separately from the formatted text.

## Layers and standards

1. MD4C determines CommonMark block boundaries and Markdown formatting.
2. Lexbor constructs HTML trees for raw HTML blocks according to
   [WHATWG HTML parsing](https://html.spec.whatwg.org/multipage/parsing.html).
   The resulting tree supplies implied end tags, void-element handling, character
   references, misnested-formatting recovery, and table foster parenting. Inline
   HTML interleaved with Markdown uses the native formatting adapter.
3. The document adapter represents paragraphs, headings, inline runs, lists,
   quotes, preformatted text, rules, links, images/pictures, and rich table cells
   with original formatted-text offsets. Semantic containers supply inherited
   alignment. Unknown constructs remain readable where possible; scripts and
   styles are never executed. The supported presentation subset is listed in
   [Compatibility](COMPATIBILITY.md#markdown-and-html).
4. DirectWrite and Direct2D lay out and paint those same native records. Search,
   selection, copying, and navigation use their text offsets. HTML export consumes
   the model rather than reparsing or inferring table structure from painted text.

## Table model

Cell placement follows [WHATWG forming a table](https://html.spec.whatwg.org/multipage/tables.html#forming-a-table):

- Cells have an anchor row/column, declared column/row spans, resolved covered row
  count, header/data identity, alignment, dimensions, and native child blocks.
- Each new anchor skips slots occupied by cells from preceding rows. Colspan
  covers consecutive columns; rowspan covers consecutive rows.
- The standard integer parser accepts a leading sign and whitespace and consumes
  the numeric prefix. Missing/invalid values default to one; colspan zero becomes
  one. Colspan saturates at 1,000 and rowspan at 65,534 without numeric overflow.
- Rowspan zero grows through the remaining rows of its own row group. Positive
  spans can create implied rows before that group ends. Group boundaries remain
  distinct, and footer groups follow the other groups.
- Invalid overlapping spans retain their declared placement and content, and the
  model records the table-model error. They are not silently shifted into a
  different valid table. The print adapter recovers overlaps by keeping the first
  cell in a conflicting interval.
- Declared `col` and `colgroup` spans contribute column slots, including slots
  beyond the final data cell. Child `col` declarations replace a colgroup's span.
  Column width hints share the same constraints as cell width hints and survive
  export. CSS styling of column groups is outside the native presentation subset.
- Nested tables have independent grids; captions remain native searchable content.

The renderer shares column constraints across the complete table. Percentage and
pixel hints reserve space before automatic columns receive the remaining width;
native cell minima prevent extreme hints from making adjacent cells unreadable. Single-row
cells establish row-height minima; spanning cells impose minimum total height on
all covered rows. Additional height is distributed equally among those rows.
[CSS table height rules](https://www.w3.org/TR/CSS22/tables.html#height-layout)
require the spanning constraint but do not prescribe its distribution. Cell
content follows top/middle/bottom alignment inside the resulting rectangle.
Borders surround cell rectangles, so internal grid lines do not cut merged cells.
Header backgrounds follow the header cell's full rectangle.

HTML export preserves cell spans, header/data tags, captions, and row groups,
including the original zero rowspan. Exported HTML can be parsed back into the
same logical grid. Native printing uses spanning column boundaries and RichEdit
vertical merge records. Printing remains the RichEdit text/image presentation
subset; it is not a browser print engine.

## Performance contract

Ordinary Markdown does not create an HTML parser or table grid. Compact block
records retain their previous 104-byte size. HTML rows without spans retain their
incremental rendering path.

The grid uses column expiry ranges, not an array of every covered slot. Layout
uses range-sum/range-add indexes for long rowspan height constraints. Storage grows
with rows, columns, and actual cells, not rows multiplied by columns.

Independent rows in a spanning table are separated into rendering bands wherever
no rowspan crosses the boundary. Bands share column constraints and row-group
identities. Only bands near the viewport require DirectWrite layouts; exports
rejoin them into one table. Connected spans are measured together. Non-source-ordered footer or caption recovery currently retains a single layout band.

Rendering caches, image loading, cancellation, bounded text children, and the
existing startup/render-worker behavior remain in place. Native table nesting is
limited to 64 cell levels, and models to 1,000,000 rows and 65,536 columns.
Exceptionally large rich table bands (over 8,192 text units, 256 children, or
256 cells) fall back to their native child blocks in source order. Text, images,
links, and formatting remain readable, while table geometry is omitted so first
paint and navigation do not require laying out the entire band.

## Verification

- Reader tests exercise standard defaults/limits, numeric recovery, combined
  spans, row-group boundaries, implied rows, footer ordering, overlaps, nested
  tables, captions, and maximum spans. Randomized small tables are checked against
  an independent dense-grid oracle used only in tests.
- Geometry tests verify that row-height constraints fit every cell, including
  long spans, without dense-grid expansion.
- Export tests check structural preservation and round-tripping.
- `tests/html-tables.md` provides a portable native rendering/navigation fixture.
- Existing CommonMark, native display/startup/search, package, and performance
  checks cover the surrounding reader/editor paths.
