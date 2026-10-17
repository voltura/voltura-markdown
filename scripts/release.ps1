param(
    [string]$NotesPath,
    [ValidatePattern('^[A-Fa-f0-9]{40}$')]
    [string]$CertificateThumbprint,
    [uri]$TimestampUrl,
    [switch]$AllowSelfSigned,
    [switch]$Unsigned,
    [string]$SigningConfig = (Join-Path $env:LOCALAPPDATA 'Voltura\Markdown\Signing\signing.json'),
    [string]$OutputDirectory,
    [ValidateRange(1, 2147483647)]
    [int]$PreviewNumber = 1,
    [switch]$Publish,
    [ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')]
    [string]$Repository,
    [switch]$Stable
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if ($Publish) {
    & (Join-Path $PSScriptRoot 'sync-source.ps1') -CheckOnly
}
$project = Get-Content -LiteralPath (Join-Path $repo 'CMakeLists.txt') -Raw
if ($project -notmatch 'project\(VolturaMarkdown VERSION (\d+\.\d+\.\d+)') { throw 'Project version missing.' }
$releaseVersion = $Matches[1]
$tag = if ($Stable) { "v$releaseVersion" } else { "v$releaseVersion-preview.$PreviewNumber" }
$title = "Voltura Markdown $releaseVersion" + $(if (!$Stable) { " Preview $PreviewNumber" })
$directory = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $repo "build\releases\$releaseVersion" }
if (Test-Path -LiteralPath $directory) { throw "Release directory already exists; inspect and archive it before preparing again: $directory" }
if (!$NotesPath) { $NotesPath = Join-Path $repo "docs\releases\$releaseVersion.md" }
$notes = Get-Content -LiteralPath $NotesPath -Raw -Encoding utf8
if ($notes -notmatch "(?m)^# Voltura Markdown $([regex]::Escape($releaseVersion))(?:\s|$)") { throw 'Notes title must match the project version.' }
if ($notes -match '(?i)<(?:version|commit|hash|bytes|YYYY-MM-DD|Brief|State)[^>]*>|\{\{|\}\}|\b(?:TODO|TBD|REPLACE_ME)\b') { throw 'Replace release-note placeholders before preparing a release.' }
if ($Stable -and $notes -match '(?im)^#.*\bpreview\b') { throw 'Stable publication needs reviewed stable release notes; provide -NotesPath after completing acceptance gates.' }
if ($Unsigned -and ($CertificateThumbprint -or $TimestampUrl -or $AllowSelfSigned)) { throw '-Unsigned cannot be combined with signing parameters.' }
if (!$Unsigned -and !$CertificateThumbprint -and !$TimestampUrl -and (Test-Path -LiteralPath $SigningConfig)) {
    $signing = Get-Content -LiteralPath $SigningConfig -Raw | ConvertFrom-Json
    $CertificateThumbprint = $signing.certificateThumbprint
    $TimestampUrl = [uri]$signing.timestampUrl
    $AllowSelfSigned = [bool]$signing.allowSelfSigned
    if (!$CertificateThumbprint -or !$TimestampUrl) { throw 'The local signing configuration is incomplete.' }
}
if ([bool]$CertificateThumbprint -ne [bool]$TimestampUrl) { throw 'Signing requires both -CertificateThumbprint and -TimestampUrl.' }
if ($CertificateThumbprint -and $CertificateThumbprint -notmatch '^[A-Fa-f0-9]{40}$') { throw 'Invalid certificate thumbprint.' }
if ($AllowSelfSigned -and !$CertificateThumbprint) { throw '-AllowSelfSigned requires a signing certificate.' }
if ($CertificateThumbprint) {
    & (Join-Path $PSScriptRoot 'sign.ps1') -Path (Join-Path $repo 'dist\vmd.exe') -CertificateThumbprint $CertificateThumbprint -TimestampUrl $TimestampUrl -ValidateOnly
}
if ($Publish -and !$Repository) { throw 'Publishing requires -Repository owner/name.' }
if ($Repository -and !$Publish) { throw '-Repository is used only with the explicit -Publish switch.' }
$revision = 'Unversioned working directory'
$sourceDirty = $null
$git = Get-Command git.exe -ErrorAction SilentlyContinue
if ($git -and (Test-Path -LiteralPath (Join-Path $repo '.git'))) {
    try {
        $head = & $git.Source -C $repo rev-parse --verify HEAD 2>$null
        if ($LASTEXITCODE -eq 0) {
            $revision = [string]$head
            $changes = & $git.Source -C $repo status --porcelain
            if ($LASTEXITCODE) { throw 'Could not inspect the Git working tree.' }
            $sourceDirty = [bool]$changes
        }
    } catch {
        if ($Publish) { throw }
        $revision = 'Unversioned working directory'
    }
}
$gh = $null
if ($Publish) {
    if ($revision -notmatch '^[a-fA-F0-9]{40}$') { throw 'Publishing requires a committed Git checkout.' }
    $changes = & $git.Source -C $repo status --porcelain
    if ($LASTEXITCODE -or $changes) { throw 'Publishing requires a clean Git working tree.' }
    $originUrl = & $git.Source -C $repo remote get-url origin
    if ($LASTEXITCODE -or $originUrl -notin @("https://github.com/$Repository.git", "https://github.com/$Repository", "git@github.com:$Repository.git")) {
        throw 'The selected release repository must match origin.'
    }
    $gh = Get-Command gh.exe -ErrorAction Stop
    & $gh.Source auth status
    if ($LASTEXITCODE) { throw 'Sign in to GitHub CLI before publishing.' }
    $existingTags = & $gh.Source api "repos/$Repository/git/matching-refs/tags/$tag" --jq '.[].ref'
    if ($LASTEXITCODE) { throw 'Cannot inspect tags in the selected existing repository.' }
    if ($existingTags -contains "refs/tags/$tag") { throw "Tag $tag already exists; inspect it and choose a new release version." }
}
New-Item -ItemType Directory -Path $directory -ErrorAction Stop | Out-Null
& (Join-Path $PSScriptRoot 'build.ps1') -Configuration Release
& python (Join-Path $PSScriptRoot 'commonmark.py')
if ($LASTEXITCODE) { throw 'CommonMark verification failed.' }
$portable = Join-Path $repo 'dist\vmd.exe'
if ($CertificateThumbprint) {
    & (Join-Path $PSScriptRoot 'sign.ps1') -Path $portable -CertificateThumbprint $CertificateThumbprint -TimestampUrl $TimestampUrl -AllowSelfSigned:$AllowSelfSigned
}
& python (Join-Path $PSScriptRoot 'verify-package.py') $portable
if ($LASTEXITCODE) { throw 'Portable package verification failed.' }
& (Join-Path $PSScriptRoot 'build-installer.ps1') -SkipBuild
$installer = Join-Path $repo "dist\VolturaMarkdown-Setup-$releaseVersion-win-x64.exe"
if ($CertificateThumbprint) {
    & (Join-Path $PSScriptRoot 'sign.ps1') -Path $installer -CertificateThumbprint $CertificateThumbprint -TimestampUrl $TimestampUrl -AllowSelfSigned:$AllowSelfSigned
}
foreach ($asset in @($portable, $installer)) {
    $metadata = [Diagnostics.FileVersionInfo]::GetVersionInfo($asset)
    if ($metadata.ProductVersion -ne $releaseVersion -or $metadata.CompanyName -ne 'Voltura AB') {
        throw "Version or company metadata differs from the release: $asset"
    }
}
$assets = @($portable, $installer)
$identity = foreach ($asset in $assets) {
    Copy-Item -LiteralPath $asset -Destination $directory
    $copy = Join-Path $directory ([IO.Path]::GetFileName($asset))
    $signature = Get-AuthenticodeSignature -LiteralPath $copy
    $verified = $null
    if ($CertificateThumbprint) {
        $verified = & (Join-Path $PSScriptRoot 'verify-signature.ps1') -Path $copy -CertificateThumbprint $CertificateThumbprint -AllowSelfSigned:$AllowSelfSigned
    }
    if (!$CertificateThumbprint -and $signature.Status -ne 'NotSigned') {
        throw "Unsigned preparation expected an unsigned executable, found $($signature.Status): $asset"
    }
    [ordered]@{
        name = [IO.Path]::GetFileName($copy)
        bytes = (Get-Item -LiteralPath $copy).Length
        sha256 = (Get-FileHash -LiteralPath $copy -Algorithm SHA256).Hash.ToLowerInvariant()
        signature = [string]$signature.Status
        trust = if ($verified) { $verified.trust } else { 'Unsigned' }
        digestVerified = [bool]$verified
        signer = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { $null }
        timestamped = [bool]$signature.TimeStamperCertificate
    }
}
$checksumPath = Join-Path $directory 'SHA256SUMS.txt'
$identity | ForEach-Object { "$($_.sha256)  $($_.name)" } | Set-Content -LiteralPath $checksumPath -Encoding ascii
$manifest = [ordered]@{
    version = $releaseVersion
    tag = $tag
    prerelease = !$Stable
    preparedAtUtc = [DateTime]::UtcNow.ToString('o')
    sourceRevision = $revision
    sourceDirty = $sourceDirty
    checks = @('Release build', 'CTest', 'CommonMark examples', 'Portable PE/metadata/imports/icons/licenses')
    assets = @($identity)
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $directory 'release-manifest.json') -Encoding utf8
$details = @('', ('Source revision: ' + $revision), '', '## Download verification', '', '| Download | Bytes | SHA-256 | Publisher signature |', '| --- | ---: | --- | --- |')
foreach ($asset in $identity) {
    $status = if ($asset.trust -eq 'Unsigned') { 'Unsigned' } elseif ($asset.trust -eq 'SelfSigned') { 'Self-signed; timestamped' } else { "Valid; $($asset.signer); timestamped" }
    $details += "| ``$($asset.name)`` | $($asset.bytes) | ``$($asset.sha256)`` | $status |"
}
$details += @('', 'SHA-256 identifies the exact download. Signing status is listed above.')
$preparedNotes = Join-Path $directory 'RELEASE-NOTES.md'
($notes.TrimEnd() + "`n" + ($details -join "`n") + "`n") | Set-Content -LiteralPath $preparedNotes -Encoding utf8
Write-Host "Prepared $tag in $directory"
Write-Host $(if ($CertificateThumbprint) { 'Both executables are signed and timestamped.' } else { 'Both executables are UNSIGNED.' })
if ($Publish) {
    & (Join-Path $PSScriptRoot 'sync-source.ps1') -CheckOnly
    $currentHead = & $git.Source -C $repo rev-parse --verify HEAD
    $changes = & $git.Source -C $repo status --porcelain
    if ($LASTEXITCODE -or $currentHead -ne $revision -or $changes) {
        throw 'Source changed during preparation. Inspect the artifacts and prepare again from a clean checkout.'
    }
    $arguments = @('release', 'create', $tag, '--repo', $Repository, '--target', $revision, '--title', $title, '--notes-file', $preparedNotes)
    if (!$Stable) { $arguments += @('--prerelease', '--latest=false') }
    $arguments += @((Join-Path $directory 'vmd.exe'), (Join-Path $directory ([IO.Path]::GetFileName($installer))), $checksumPath)
    & $gh.Source @arguments
    if ($LASTEXITCODE) { throw 'GitHub release creation failed. Prepared artifacts remain available for inspection.' }
    if ($Stable) {
        & (Join-Path $PSScriptRoot 'publish-pages.ps1') -Repository $Repository -Publish
    }
} else {
    Write-Host 'Preparation only: no Git tag, push, repository, or GitHub release was created.'
}
