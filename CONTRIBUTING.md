# Contributing

Keep the reader fast, compact, and read-only.

Contributions use the [MIT License](LICENSE). Preserve [third-party notices](THIRD-PARTY-NOTICES.md), including fixture attribution. Discuss substantial changes in an issue first.

## Report a problem

Use the bug-report template. Include the Windows version, app version, display scale, steps, and expected/actual result. Provide a small Markdown reproduction when possible. Remove private text, local usernames, credentials, and confidential paths. For performance reports, include document size/shape, hardware, cache conditions, and whether timings include OS startup/loading.

For security-sensitive findings, follow [SECURITY.md](SECURITY.md) instead of a public issue.

## Development

Follow the [build guide](docs/BUILDING.md), [code style guide](docs/CODE-STYLE.md), and [source ownership map](docs/ARCHITECTURE.md). Keep source under `src/`, fixtures under `tests/`, and generated outputs under ignored `build/` or `dist/`. Use `.clang-format` for application C++. Preserve vendored upstream sources/notices unless deliberately updating their pinned revisions.

Keep changes focused and retain the read-only contract. Avoid per-block widgets, unbounded caches, UI-thread I/O, and animation that repeatedly repaints unrelated controls. Changes affecting rendering, shell integration, clipboard, images, or performance need corresponding evidence. Document visible behavior in the user guide and changelog.

Before review, run the relevant tests. For parser changes run the CommonMark suite; for packaging/resource changes run the package verifier. Scope claims to the environments actually tested. Do not include build outputs, local screenshots, personal documents, or preference files in a pull request.
