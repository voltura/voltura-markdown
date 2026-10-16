# Repository map

## Folder map

| Folder | Contents |
| --- | --- |
| [.github/](../.github/) | Issue and pull-request templates |
| [assets/](../assets/) | Embedded Windows resources and current icon artwork |
| [docs/](./) | Focused help and maintainer documentation |
| [scripts/](../scripts/) | Build, installer, format, verification, benchmark, artwork tools |
| [site/](../site/) | End-user product page and styles; see [local Pages workflow](PAGES.md) |
| [src/](../src/) | Native application modules; see [code ownership/dataflow](ARCHITECTURE.md) |
| [tests/](../tests/) | Native/integration tests and pinned Markdown fixtures |
| [third_party/](../third_party/) | Vendored parser/SVG sources and upstream notices |

## Document map

| Document | Audience and purpose |
| --- | --- |
| [README](../README.md) | Visitors: purpose, download, use, build entry point |
| [User guide](USER-GUIDE.md) | Readers: controls, shell actions, preferences, troubleshooting |
| [Compatibility](COMPATIBILITY.md) | Readers/authors: supported syntax, encodings, images, limits |
| [Build guide](BUILDING.md) | Developers: prerequisites and verification commands |
| [Product site](PAGES.md) | Maintainers: local preview/build and explicit branch-based Pages publication |
| [Architecture](ARCHITECTURE.md) | Developers: source ownership and document/rendering flow |
| [Code style](CODE-STYLE.md) | Contributors: formatting conventions and checks |
| [Performance](../PERFORMANCE.md) | Reviewers: measurements, methods, remaining validation |
| [Changelog](../CHANGELOG.md) | Users: visible changes by release |
| [Release procedure](RELEASING.md) | Maintainers: gates, artifact identity, publication steps |
| [Release notes template](RELEASE-NOTES-TEMPLATE.md) | Maintainers: clear executable download/use instructions |
| [Contributing](../CONTRIBUTING.md) / [Security](../SECURITY.md) | Contributors/reporters: collaboration and private reporting |
| [License](../LICENSE) / [Third-party notices](../THIRD-PARTY-NOTICES.md) | Everyone: application terms and upstream attribution |
| [Artwork notes](../assets/ICON-NOTES.md) | Maintainers: provenance and reproducible icon export |

## Documentation practice

Keep one owner per topic. README covers download/start; the separate website explains the product to readers; focused guides cover details. Use relative links, short headings, and tables for reference material. Update help with behavior changes and the changelog with visible changes. Keep diagnostic logs and development history out of public guides.

## Public repository checklist

- Review tracked files for private paths, documents, logs, and credentials.
- Retain MIT and dependency/fixture notices.
- Description: **Fast, portable, read-only Markdown viewer for Windows.**
- Topics: `markdown`, `windows`, `cpp20`, `win32`, `direct2d`, `portable`.
- Configure issues and private vulnerability reporting to match SECURITY.md.
- Complete the [release checklist](RELEASING.md) before publishing binaries.

Build/release/signing/site work uses local scripts. GitHub Actions is not required. Generated `build/` and `dist/` stay ignored; binaries belong in Releases.
