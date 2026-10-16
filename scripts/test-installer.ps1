$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$compiler = Get-Command makensis.exe -ErrorAction SilentlyContinue
if (!$compiler) {
    $compiler = Get-Command (Join-Path ${env:ProgramFiles(x86)} 'NSIS\makensis.exe') -ErrorAction Stop
}
$project = Get-Content -LiteralPath (Join-Path $repo 'CMakeLists.txt') -Raw
if ($project -notmatch 'project\(VolturaMarkdown VERSION (\d+\.\d+\.\d+)') {
    throw 'Project version missing.'
}
$releaseVersion = $Matches[1]
$identity = [Guid]::NewGuid().ToString('N')
$productKey = "VolturaMarkdown-Test-$identity"
$shortcutName = "Voltura Markdown Test $identity"
$installer = Join-Path $repo "build\installer-test-$identity.exe"
$installDir = Join-Path $repo "build\install test $identity"
$relocatedDir = Join-Path $repo "build\install moved $identity"
$registryPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$productKey"
$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) "$shortcutName.lnk"
$preferences = Join-Path $env:LOCALAPPDATA 'Voltura\Markdown\preferences.ini'
$preferencesBefore = if (Test-Path -LiteralPath $preferences) {
    (Get-FileHash -LiteralPath $preferences -Algorithm SHA256).Hash
}

# A separate compiled identity isolates the test's Installed apps entry and shortcut.
& $compiler.Source /WX /V2 "/DVERSION=$releaseVersion" "/DOUTPUT=$installer" "/DPRODUCT_KEY=$productKey" "/DSHORTCUT_NAME=$shortcutName" (Join-Path $PSScriptRoot 'installer.nsi')
if ($LASTEXITCODE) { throw 'Smoke-test installer build failed.' }

try {
    $process = Start-Process -FilePath $installer -ArgumentList @('/S', "/D=$installDir") -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode) { throw "Install failed: $($process.ExitCode)" }
    $installed = Join-Path $installDir 'vmd.exe'
    $expected = (Get-FileHash -LiteralPath (Join-Path $repo 'dist\vmd.exe') -Algorithm SHA256).Hash
    if ((Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash -ne $expected) {
        throw 'Installed executable differs from the verified portable binary.'
    }
    if (!(Test-Path -LiteralPath $shortcut) -or !(Test-Path -LiteralPath $registryPath)) {
        throw 'Installer shortcut or Installed apps entry missing.'
    }
    foreach ($file in @('Uninstall.exe', 'LICENSE', 'THIRD-PARTY-NOTICES.md', 'licenses\MD4C-LICENSE.md', 'licenses\NanoSVG-LICENSE.txt')) {
        if (!(Test-Path -LiteralPath (Join-Path $installDir $file))) { throw "Missing installed file: $file" }
    }
    $marker = Join-Path $installDir 'keep-me.md'
    Set-Content -LiteralPath $marker -Value '# User document' -Encoding utf8

    $lockedFile = [IO.File]::Open($installed, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $process = Start-Process -FilePath (Join-Path $installDir 'Uninstall.exe') -ArgumentList @('/S', "_?=$installDir") -Wait -PassThru -WindowStyle Hidden
        if ($process.ExitCode -eq 0 -or !(Test-Path -LiteralPath $installed) -or !(Test-Path -LiteralPath $registryPath) -or !(Test-Path -LiteralPath $shortcut)) {
            throw 'Locked-file uninstall did not fail safely.'
        }
    }
    finally {
        $lockedFile.Dispose()
    }

    $process = Start-Process -FilePath $installer -ArgumentList @('/S', "/D=$installDir") -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -or !(Test-Path -LiteralPath $marker)) { throw 'Upgrade failed or removed a user document.' }

    $process = Start-Process -FilePath $installer -ArgumentList @('/S', "/D=$relocatedDir") -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode) { throw 'Relocated install failed.' }
    $process = Start-Process -FilePath (Join-Path $installDir 'Uninstall.exe') -ArgumentList @('/S', "_?=$installDir") -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode) { throw "Old-copy uninstall failed: $($process.ExitCode)" }
    $newInstalled = Join-Path $relocatedDir 'vmd.exe'
    if (!(Test-Path -LiteralPath $newInstalled) -or !(Test-Path -LiteralPath $shortcut)) {
        throw 'Old uninstaller damaged the relocated installation.'
    }
    if ((Get-ItemProperty -LiteralPath $registryPath).InstallLocation -ne $relocatedDir) {
        throw 'Old uninstaller removed the relocated Installed apps entry.'
    }
    $shell = New-Object -ComObject WScript.Shell
    if ($shell.CreateShortcut($shortcut).TargetPath -ne $newInstalled) {
        throw 'Shortcut no longer targets the relocated installation.'
    }
    $process = Start-Process -FilePath (Join-Path $relocatedDir 'Uninstall.exe') -ArgumentList @('/S', "_?=$relocatedDir") -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode) { throw "Uninstall failed: $($process.ExitCode)" }
    if ((Test-Path -LiteralPath $newInstalled) -or (Test-Path -LiteralPath $installed) -or (Test-Path -LiteralPath $shortcut) -or (Test-Path -LiteralPath $registryPath)) {
        throw 'Uninstall left application files, shortcut, or Installed apps entry.'
    }
    if (!(Test-Path -LiteralPath $marker)) { throw 'Uninstall removed a user document.' }
    $preferencesAfter = if (Test-Path -LiteralPath $preferences) {
        (Get-FileHash -LiteralPath $preferences -Algorithm SHA256).Hash
    }
    if ($preferencesAfter -ne $preferencesBefore) { throw 'Reader preferences changed during installer test.' }
    Write-Host 'PASS: install, locked-file recovery, upgrade, relocation, old-copy uninstall ownership, binary identity, notices, shortcut, uninstall, retained documents/preferences.'
}
finally {
    # Never recurse through an installer-selected directory or remove unrelated files.
    if (Test-Path -LiteralPath $registryPath) {
        Write-Warning "A failed smoke test left its isolated entry: $registryPath"
    }
    else {
        foreach ($directory in @($installDir, $relocatedDir)) {
            foreach ($name in @('keep-me.md', 'Uninstall.exe')) {
                $path = Join-Path $directory $name
                if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
            }
            if ((Test-Path -LiteralPath $directory) -and !(Get-ChildItem -LiteralPath $directory -Force)) {
                Remove-Item -LiteralPath $directory
            }
        }
    }
}
