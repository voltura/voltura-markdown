# Local releases and signing

Required: Windows x64, the [build toolchain](BUILDING.md), Python, NSIS 3, and GitHub CLI for publication. Close the reader and run from the repository root.

## Prepare assets

```powershell
.\scripts\release.ps1
```

Builds/tests the app, runs CommonMark/package checks, builds Setup, and writes unsigned preview assets under ignored `build/releases/<version>/`: both executables, `SHA256SUMS.txt`, `RELEASE-NOTES.md`, and `release-manifest.json`. Preparation does not publish or modify Git.

Version comes from `CMakeLists.txt`; synchronize executable metadata, package-verifier expectations, and `docs/releases/<version>.md`. Use `-NotesPath` for alternative reviewed notes. Placeholders are rejected. Existing release directories are not overwritten; inspect partial output after a failed run. Never reuse a published version for changed assets.

## Sign locally

Use an existing CurrentUser Personal code-signing certificate with its private key, plus your provider's RFC 3161 timestamp URL:

```powershell
Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
    Select-Object Subject, Thumbprint, NotAfter
$thumbprint = Read-Host 'Certificate thumbprint'
$timestamp = Read-Host 'RFC 3161 timestamp URL'
.\scripts\release.ps1 -CertificateThumbprint $thumbprint -TimestampUrl $timestamp
```

The reader is signed before packaging; Setup is signed separately. Both signatures and timestamps are validated before hashing. Omit signing parameters for explicitly unsigned assets. Keep certificates, private keys, and credentials outside the repository. See [SignTool](https://learn.microsoft.com/en-us/windows/win32/seccrypto/signtool).

Check signing configuration without changing a file:

```powershell
.\scripts\sign.ps1 -Path .\dist\vmd.exe -CertificateThumbprint $thumbprint -TimestampUrl $timestamp -ValidateOnly
```

## Publish explicitly

Requires an existing repository, authenticated GitHub CLI, and a clean committed checkout. Start with a fresh release output directory:

```powershell
$repository = Read-Host 'GitHub repository (owner/name)'
.\scripts\release.ps1 -Publish -Repository $repository
```

Add signing parameters for signed assets. Default publication uses `v<version>-preview.1`, marked prerelease and not latest. Use `-Stable` only after all gates below pass and stable notes are reviewed; it publishes `v<version>`. The script rechecks the source revision before publication.

After a network failure, inspect the remote release before retrying. Re-download published executables and compare their SHA-256 values. Do not silently replace assets under an existing version.

## Release checklist

- [ ] Review public files, metadata, MIT/dependency notices, and private security reporting.
- [ ] Synchronize version/notes; date the changelog when publishing.
- [ ] Run formatting, native/CommonMark/package checks, and both UI exit paths.
- [ ] Run isolated installer lifecycle tests and inspect wizard appearance/scaling.
- [ ] Verify clean Windows 10/11 execution without developer runtimes.
- [ ] Verify installation, upgrade, removal, retained documents/preferences, and optional default selection in an isolated profile. Leave developer associations unchanged.
- [ ] Test physical mixed-DPI movement, fullscreen restoration, keyboard/copy/search, and both themes.
- [ ] Measure cold/warm startup, memory, and actual scrolling/frame delivery. Targets: small files 500 ms cold / 150 ms warm; 10 MiB 2 s cold / 1 s warm. Executable target under 5 MB, ceiling 10 MB.
- [ ] Record source/toolchain, checks, artifact identity, and signing state in the manifest.
- [ ] Re-download published assets and verify hashes.

Use [Building](BUILDING.md) for commands and [Performance](../PERFORMANCE.md) for methodology. Outstanding checks are in [Compatibility](COMPATIBILITY.md#platform). Publish a clearly labeled preview while gates remain unresolved.

Release assets contain executable packages and metadata only: exclude PDBs, tests, benchmarks, and build trees. Use the [notes template](RELEASE-NOTES-TEMPLATE.md) for concise download/use instructions. All preparation, signing, and publication run locally; GitHub Actions is not required.
