param(
    [switch]$Check,
    [string]$ClangFormat = 'clang-format'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

if (!(Get-Command $ClangFormat -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $ClangFormat = Join-Path $visualStudio 'VC\Tools\Llvm\x64\bin\clang-format.exe'
}
if (!(Get-Command $ClangFormat -ErrorAction SilentlyContinue)) {
    throw 'Install clang-format 22.1.3, or pass its path with -ClangFormat.'
}
$version = & $ClangFormat --version
if ($version -notmatch 'version 22\.1\.3\b') {
    throw "Expected clang-format 22.1.3; found $version"
}

Push-Location $repo
try {
    $sources = Get-ChildItem -LiteralPath src, tests, scripts -Recurse -File |
        Where-Object { $_.Extension -in '.cpp', '.h' } |
        ForEach-Object { $_.FullName }
    if ($Check) {
        & $ClangFormat --dry-run --Werror @sources
    }
    else {
        & $ClangFormat -i @sources
    }
    if ($LASTEXITCODE) { throw 'C++ formatting failed.' }

    if ($Check) {
        python -m black --check scripts
    }
    else {
        python -m black scripts
    }
    if ($LASTEXITCODE) { throw 'Python formatting failed. Install requirements-dev.txt.' }
}
finally {
    Pop-Location
}
