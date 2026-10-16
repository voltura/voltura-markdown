# Third-party notices

Voltura Markdown's own application source, documentation, and current artwork are licensed under the [MIT License](LICENSE), copyright © 2026 Voltura AB. The components below retain their respective licenses and attribution. They are not relicensed by the application's MIT license.

## MD4C

- Source: [mity/md4c](https://github.com/mity/md4c).
- Pinned revision: `60e8065cfccd5d7ac561115f9a9bfbdf811f5cc9`.
- Vendored files: `third_party/md4c/`.
- Copyright and complete MIT terms: [retained upstream license](third_party/md4c/LICENSE.md).
- Included in the application; its notice is embedded in About / open-source licenses.

## NanoSVG

- Source: [memononen/nanosvg](https://github.com/memononen/nanosvg).
- Pinned revision: `239e102ec2c691f2902e20ace2ed36ee4a35cfe6`.
- Vendored files: `third_party/nanosvg/`.
- Copyright and complete zlib terms: [retained upstream license](third_party/nanosvg/LICENSE.txt).
- Included in the application; its notice is embedded in About / open-source licenses.

## CommonMark specification examples

`tests/commonmark-0.31.2.json` contains the 652 examples from **CommonMark Spec 0.31.2** (2024-01-28) by **John MacFarlane**. The specification is licensed under [Creative Commons Attribution-ShareAlike 4.0 International (CC BY-SA 4.0)](https://creativecommons.org/licenses/by-sa/4.0/); see the [legal code](https://creativecommons.org/licenses/by-sa/4.0/legalcode.en) and [upstream licensing notice](https://github.com/commonmark/commonmark-spec/blob/0.31.2/LICENSE).

- Specification and attribution: [CommonMark Spec 0.31.2](https://spec.commonmark.org/0.31.2/).
- Original machine-readable examples: [spec.json](https://spec.commonmark.org/0.31.2/spec.json).
- Example content matches the official JSON; serialization may differ.
- The fixture retains CC BY-SA 4.0 terms, including attribution and the applicable share-alike requirement for adaptations. It is test data and is not embedded in `vmd.exe`.

This attribution distinguishes the specification fixture from the application's independently authored verification scripts and native application code.

## Complete bundled component licenses

The full dependency notices below are included so this document remains useful beside an installed executable without a source checkout.

### MD4C — MIT

```text
# The MIT License (MIT)

Copyright © 2016-2026 Martin Mitáš

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the “Software”),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.
```

### NanoSVG — zlib

```text
Copyright (c) 2013-14 Mikko Mononen memon@inside.org

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
claim that you wrote the original software. If you use this software
in a product, an acknowledgment in the product documentation would be
appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```
