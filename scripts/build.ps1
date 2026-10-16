param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Install Visual Studio with Desktop development with C++ and CMake tools.' }
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake -S $repo -B (Join-Path $repo 'build') -G 'Visual Studio 18 2026' -A x64
if ($LASTEXITCODE) { throw 'CMake configure failed' }
& $cmake --build (Join-Path $repo 'build') --config $Configuration --parallel
if ($LASTEXITCODE) { throw 'Build failed' }
& (Join-Path (Split-Path $cmake) 'ctest.exe') --test-dir (Join-Path $repo 'build') -C $Configuration --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed' }
if ($Configuration -eq 'Release') {
    $distribution = Join-Path $repo 'dist'
    New-Item -ItemType Directory -Force $distribution | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo 'build\Release\vmd.exe') -Destination (Join-Path $distribution 'vmd.exe')
    Write-Host "Single-file distribution: $distribution\vmd.exe"
}
