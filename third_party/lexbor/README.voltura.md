# Lexbor HTML parser

Upstream: https://github.com/lexbor/lexbor
Version: v3.0.0
Source archive: https://github.com/lexbor/lexbor/archive/refs/tags/v3.0.0.zip
Archive SHA-256: 7a791c914cadc0ac24ae0c2421d53ea8e92d840a99f4e01ee1519b8d243eff68

Unmodified upstream core, dom, html, ns, tag, and ports modules. Other modules are
omitted. CMake builds the HTML parser and its dependencies statically, with the
Windows port. No network access or separate runtime is needed to build or run.
LICENSE and NOTICE retain the upstream Apache 2.0 license and attribution.

The raw HTML blocks identified by MD4C are parsed with HTML5 tree construction.
VMD presents the resulting tree with its native renderer; this does not make VMD
a complete browser or a CSS layout engine. The normal Markdown path does not
create an HTML parser.
