# Local releases and signing

Required: Windows x64, the [build toolchain](BUILDING.md), Python, NSIS 3, and GitHub CLI for publication. Close the reader and run from the repository root.

## Prepare assets

```powershell
.\scripts\release.ps1
```

Builds/tests the app, runs CommonMark/package checks, builds Setup, and writes preview assets under ignored `build/releases/<version>/`: both executables, `SHA256SUMS.txt`, `RELEASE-NOTES.md`, and `release-manifest.json`. A configured local certificate signs both executables automatically. Preparation does not publish or modify Git.

Version comes from `CMakeLists.txt`; synchronize executable metadata, package-verifier expectations, and `docs/releases/<version>.md`. Use `-NotesPath` for alternative reviewed notes. Placeholders are rejected. Existing release directories are not overwritten; inspect partial output after a failed run. Never reuse a published version for changed assets.

## Sign locally

Create a local self-signed certificate once:

```powershell
.\scripts\setup-signing.ps1
.\scripts\release.ps1
```

Setup reuses the configured key on subsequent runs. Its non-exportable RSA private key stays in Windows CurrentUser Personal; the public certificate and configuration are stored under `%LOCALAPPDATA%\Voltura\Markdown\Signing`, outside the repository. No trusted-root or trusted-publisher stores are changed. A self-signed signature does not establish public publisher trust or remove Windows warnings.

Release preparation uses SHA-256 and an RFC 3161 timestamp, signs the reader before embedding it in Setup, then signs Setup separately. Both file digests, signer identities, and timestamps are verified before hashing. Verification only permits the expected untrusted-root result for the explicitly configured self-signed key; other signature errors fail.

To prepare another local build without overwriting existing output:

```powershell
.\scripts\release.ps1 -OutputDirectory .\build\releases\1.0.0-self-signed
```

Use `-Unsigned` to explicitly bypass local signing. For a different existing CurrentUser code-signing certificate, pass its thumbprint and RFC 3161 timestamp URL:

```powershell
Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
    Select-Object Subject, Thumbprint, NotAfter
$thumbprint = Read-Host 'Certificate thumbprint'
$timestamp = Read-Host 'RFC 3161 timestamp URL'
.\scripts\release.ps1 -CertificateThumbprint $thumbprint -TimestampUrl $timestamp
```

Explicit certificates require trusted verification unless `-AllowSelfSigned` is also supplied. Keep private keys and credentials outside the repository. The release manifest distinguishes self-signed, trusted, and unsigned assets. See [SignTool](https://learn.microsoft.com/en-us/windows/win32/seccrypto/signtool).

Check signing configuration without changing a file:

```powershell
.\scripts\sign.ps1 -Path .\dist\vmd.exe -CertificateThumbprint $thumbprint -TimestampUrl $timestamp -ValidateOnly
```

Run `./scripts/test-signing.ps1` after building to check timestamping, signer pinning, and rejection of modified executables using the local signing configuration.

## Publish explicitly

Requires an existing repository, authenticated GitHub CLI, and a clean committed checkout. Start with a fresh release output directory:

```powershell
$repository = Read-Host 'GitHub repository (owner/name)'
.\scripts\release.ps1 -Publish -Repository $repository
```

Add signing parameters for signed assets. Default publication uses `v<version>-preview.1`, marked prerelease and not latest. Select a later preview with `-PreviewNumber 2` and its reviewed `-NotesPath`; use a fresh `-OutputDirectory` when the version already has prepared assets. Use `-Stable` only after all gates below pass and stable notes are reviewed; it publishes `v<version>`. The script rechecks the source revision before publication.

After a network failure, inspect the remote release before retrying. Re-download published executables and compare their SHA-256 values. Do not silently replace assets under an existing version.

## Release checklist

- [ ] Review public files, metadata, MIT/dependency notices, and private security reporting.
- [ ] Synchronize version/notes; date the changelog when publishing.
- [ ] Run formatting, native/CommonMark/package checks, and both UI exit paths.
- [ ] Run isolated installer lifecycle tests and inspect wizard appearance/scaling.
- [x] Maintainer reports clean Windows execution without developer runtimes verified (2026-10-17).
- [x] Installer lifecycle automation passes; maintainer reports default association checks completed (2026-10-17). Developer associations remain unchanged.
- [x] Simulated mixed-DPI changes, fullscreen restoration, keyboard/copy/search, and both themes pass. Physical monitor movement remains additional hardware coverage.
- [x] Startup, memory and frame delivery recorded in PERFORMANCE.md. Separate reboot launches pass at 232 ms / 564 ms; 59.6 displayed updates/s measured. Small-file warm target miss accepted by maintainer for 1.0; hardware rendering retained. Executable below 1 MiB.
- [ ] Record source/toolchain, checks, artifact identity, and signing state in the manifest.
- [ ] Re-download published assets and verify hashes.

Use [Building](BUILDING.md) for commands and [Performance](../PERFORMANCE.md) for methodology. Outstanding checks are in [Compatibility](COMPATIBILITY.md#platform). Publish a clearly labeled preview while gates remain unresolved.

Release assets contain executable packages and metadata only: exclude PDBs, tests, benchmarks, and build trees. Use the [notes template](RELEASE-NOTES-TEMPLATE.md) for concise download/use instructions. All preparation, signing, and publication run locally; GitHub Actions is not required.

Stable publication also rebuilds and publishes the product page locally. Download links use GitHub's latest release route and the current versioned installer filename. Publish each stable version through this script so the page and installer remain synchronized.
