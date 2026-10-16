param([switch]$SkipBuild)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (!$SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1')
}

$compiler = Get-Command makensis.exe -ErrorAction SilentlyContinue
if (!$compiler) {
    $compiler = Get-Command (Join-Path ${env:ProgramFiles(x86)} 'NSIS\makensis.exe') -ErrorAction SilentlyContinue
}
if (!$compiler) {
    throw 'Install NSIS 3 Unicode, or add makensis.exe to PATH.'
}
$project = Get-Content -LiteralPath (Join-Path $repo 'CMakeLists.txt') -Raw
if ($project -notmatch 'project\(VolturaMarkdown VERSION (\d+\.\d+\.\d+)') {
    throw 'Project version missing.'
}
$releaseVersion = $Matches[1]
if (!(Test-Path -LiteralPath (Join-Path $repo 'dist\vmd.exe'))) {
    throw 'Build the portable executable before packaging.'
}
$artwork = [IO.File]::ReadAllBytes((Join-Path $repo 'assets\installer-banner.bmp'))
if ([BitConverter]::ToInt32($artwork, 18) -ne 1312 -or [BitConverter]::ToInt32($artwork, 22) -ne 2512) {
    throw 'Installer artwork must be 1312 x 2512 pixels to match the high-quality renderer.'
}

& $compiler.Source /WX "/DVERSION=$releaseVersion" (Join-Path $PSScriptRoot 'installer.nsi')
if ($LASTEXITCODE) { throw 'Installer build failed.' }
Write-Host "Installer ready: $repo\dist\VolturaMarkdown-Setup-$releaseVersion-win-x64.exe"
