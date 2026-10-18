param([switch]$SkipSigning, [switch]$Interactive)
$ErrorActionPreference = 'Stop'
if (!$Interactive) {
    . (Join-Path $PSScriptRoot 'invoke-test-desktop.ps1')
    Invoke-IsolatedGuiTest -Script $PSCommandPath -Parameters $PSBoundParameters
    return
}
$repo = Split-Path -Parent $PSScriptRoot
$identity = [Guid]::NewGuid().ToString('N')
$folder = Join-Path $repo "build\update checks å $identity"
New-Item -ItemType Directory -Path $folder | Out-Null
$runner = Join-Path $folder 'update_tests.exe'
$fixture = Join-Path $folder 'payload.exe'
Copy-Item -LiteralPath (Join-Path $repo 'build\Release\update_tests.exe') -Destination $runner
Copy-Item -LiteralPath (Join-Path $repo 'build\Release\update_fixture.exe') -Destination $fixture
if ($SkipSigning) {
    & $runner
    if ($LASTEXITCODE) { throw 'Updater policy tests failed.' }
    return
}
$configPath = Join-Path $env:LOCALAPPDATA 'Voltura\Markdown\Signing\signing.json'
$signing = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
foreach ($file in @($runner, $fixture)) {
    & (Join-Path $PSScriptRoot 'sign.ps1') -Path $file -CertificateThumbprint $signing.certificateThumbprint -TimestampUrl $signing.timestampUrl -AllowSelfSigned:([bool]$signing.allowSelfSigned)
}
& $runner --signed-fixture $fixture
if ($LASTEXITCODE) { throw 'Signed updater staging tests failed.' }

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class UpdateTestWindows {
    delegate bool Visitor(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumWindows(Visitor visit, IntPtr data);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr window, System.Text.StringBuilder name, int count);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr window, uint msg, IntPtr w, IntPtr l);
    public static IntPtr Find(uint wanted) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((window, data) => { uint pid; GetWindowThreadProcessId(window, out pid); var name = new System.Text.StringBuilder(128); GetClassNameW(window, name, 128); if (pid == wanted && name.ToString() == "VolturaMarkdownUpdateSession") found = window; return true; }, IntPtr.Zero);
        return found;
    }
}
'@
function Wait-Window($process) {
    $until = [DateTime]::UtcNow.AddSeconds(10)
    do {
        $window = [UpdateTestWindows]::Find($process.Id)
        if ($window -ne [IntPtr]::Zero) { return $window }
        if ($process.HasExited) { throw 'Process exited before its session window appeared.' }
        Start-Sleep -Milliseconds 20
    } while ([DateTime]::UtcNow -lt $until)
    throw 'Session window did not appear.'
}
function Start-Helper($target, $asset = $fixture, [switch]$Setup) {
    $arguments = @('--prepare-helper', $target, $asset)
    if ($Setup) { $arguments += 'setup' }
    $helperPath = & $runner @arguments
    if ($LASTEXITCODE) { throw 'Could not prepare isolated helper fixture.' }
    $helperPath = [string]$helperPath
    $process = Start-Process -FilePath $helperPath -ArgumentList @('--apply-update', ('"' + $target + '"'), '4294967294') -WindowStyle Hidden -PassThru
    return @{ Process = $process; Path = $helperPath; Root = (Split-Path -Parent (Split-Path -Parent $helperPath)) }
}
function Wait-Helper($helper, [bool]$success) {
    if (!$helper.Process.WaitForExit(30000)) { throw 'Helper did not exit.' }
    if (($helper.Process.ExitCode -eq 0) -ne $success) {
        Get-Content -LiteralPath (Join-Path $helper.Root 'updates.log') -ErrorAction SilentlyContinue | Write-Host
        throw "Unexpected helper result: $($helper.Process.ExitCode)"
    }
}
$sourceHash = (Get-FileHash -LiteralPath $runner -Algorithm SHA256).Hash
$newHash = (Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash
$targets = @()
$helpers = @()
$readers = @()
$oldTestProfile = $env:VMD_UPDATES_TEST_PROFILE
$env:VMD_UPDATES_TEST_PROFILE = Join-Path $folder 'preferences.ini'
Set-Content -LiteralPath $env:VMD_UPDATES_TEST_PROFILE -Value "[reader]`nautomaticUpdates=1" -Encoding unicode
try {
    # Actual replacement of an isolated renamed portable executable, using signed fixtures.
    $target = Join-Path $folder 'Renamed portable reader.exe'; $targets += $target
    Copy-Item -LiteralPath $runner -Destination $target
    $helper = Start-Helper $target; $helpers += $helper
    Wait-Helper $helper $true
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $newHash) { throw 'Portable replacement differs from signed update.' }
    Write-Host 'PASS: real signed portable helper replacement, no restart.'

    $guiTarget = Join-Path $folder 'GUI reader.exe'
    Copy-Item -LiteralPath (Join-Path $repo 'build\Release\vmd-test.exe') -Destination $guiTarget
    & (Join-Path $PSScriptRoot 'sign.ps1') -Path $guiTarget -CertificateThumbprint $signing.certificateThumbprint -TimestampUrl $signing.timestampUrl -AllowSelfSigned:([bool]$signing.allowSelfSigned)
    $helper = Start-Helper $guiTarget; $helpers += $helper
    Wait-Helper $helper $true
    if ((Get-FileHash -LiteralPath $guiTarget -Algorithm SHA256).Hash -ne $newHash) { throw 'GUI helper entry did not apply the signed payload.' }
    Write-Host 'PASS: real GUI reader executable runs the internal updater entry before reader initialization.'

    foreach ($scenario in @('shutdown', 'cancelled-shutdown', 'reopen', 'locked', 'opted-out', 'multiple-readers')) {
        $target = Join-Path $folder "$scenario reader.exe"; $targets += $target
        Copy-Item -LiteralPath $runner -Destination $target
        $reader = $null; $locked = $null
        $secondReader = $null
        if ($scenario -notin @('locked', 'opted-out')) {
            $reader = Start-Process -FilePath $target -ArgumentList '--hold' -WindowStyle Hidden -PassThru
            $readers += $reader
            $readerWindow = Wait-Window $reader
            if ($scenario -eq 'multiple-readers') {
                $secondReader = Start-Process -FilePath $target -ArgumentList '--hold' -WindowStyle Hidden -PassThru
                $readers += $secondReader
                $secondWindow = Wait-Window $secondReader
            }
        } elseif ($scenario -eq 'locked') {
            $locked = [IO.File]::Open($target, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        } else {
            Set-Content -LiteralPath $env:VMD_UPDATES_TEST_PROFILE -Value "[reader]`nautomaticUpdates=0" -Encoding unicode
        }
        try {
            $helper = Start-Helper $target; $helpers += $helper
            if ($scenario -notin @('locked', 'opted-out')) {
                $window = Wait-Window $helper.Process
                if ($scenario -eq 'reopen') {
                    $reopened = Start-Process -FilePath $target -ArgumentList '--hold' -WindowStyle Hidden -PassThru
                    $readers += $reopened
                    $reopenedWindow = Wait-Window $reopened
                    [void][UpdateTestWindows]::SendMessageW($reopenedWindow, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
                    [void]$reopened.WaitForExit(5000)
                } elseif ($scenario -ne 'multiple-readers') {
                    [void][UpdateTestWindows]::SendMessageW($window, 0x11, [IntPtr]::Zero, [IntPtr]::Zero)
                    if ($scenario -eq 'cancelled-shutdown') {
                        [void][UpdateTestWindows]::SendMessageW($window, 0x16, [IntPtr]::Zero, [IntPtr]::Zero)
                    }
                }
                [void][UpdateTestWindows]::SendMessageW($readerWindow, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
                [void]$reader.WaitForExit(5000)
                if ($secondReader) {
                    Start-Sleep -Milliseconds 300
                    if ($helper.Process.HasExited) { throw 'Helper did not wait for every reader.' }
                    [void][UpdateTestWindows]::SendMessageW($secondWindow, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
                    [void]$secondReader.WaitForExit(5000)
                }
            }
            $success = $scenario -eq 'multiple-readers'
            Wait-Helper $helper $success
            $expectedHash = if ($success) { $newHash } else { $sourceHash }
            if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $expectedHash) { throw "Unexpected target after $scenario." }
            Write-Host "PASS: $scenario installation policy."
        } finally {
            if ($locked) { $locked.Dispose() }
            Set-Content -LiteralPath $env:VMD_UPDATES_TEST_PROFILE -Value "[reader]`nautomaticUpdates=1" -Encoding unicode
        }
    }
    $tampered = Join-Path $folder 'tampered.exe'
    Copy-Item -LiteralPath $fixture -Destination $tampered
    $bytes = [IO.File]::ReadAllBytes($tampered)
    $bytes[512] = $bytes[512] -bxor 1
    [IO.File]::WriteAllBytes($tampered, $bytes)
    & $runner --verify-signature $tampered
    if ($LASTEXITCODE -eq 0) { throw 'Tampered signature accepted.' }
    Write-Host 'PASS: modified signed executable rejected.'

    foreach ($mode in @('handoff', 'late-session')) {
        $target = Join-Path $folder "$mode reader.exe"
        Copy-Item -LiteralPath $runner -Destination $target
        $options = @('--handoff-fixture', $fixture)
        if ($mode -eq 'late-session') { $options += '--session-end' }
        $handoffRoot = [string](& $target @options)
        if ($LASTEXITCODE) { throw 'Reader handoff fixture failed.' }
        $until = [DateTime]::UtcNow.AddSeconds(15)
        $logPath = Join-Path $handoffRoot 'updates.log'
        do {
            Start-Sleep -Milliseconds 50
            $log = Get-Content -LiteralPath $logPath -Raw -ErrorAction SilentlyContinue
            if ($log -match 'installed successfully|Update unavailable|Installation deferred') { break }
        } while ([DateTime]::UtcNow -lt $until)
        $hash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
        if (($mode -eq 'handoff' -and $hash -ne $newHash) -or ($mode -eq 'late-session' -and $hash -ne $sourceHash)) {
            throw "Reader $mode failed: $log"
        }
        foreach ($directory in Get-ChildItem -LiteralPath $handoffRoot -Directory) {
            foreach ($name in @('payload.exe', 'helper.exe', 'download.part')) {
                $path = Join-Path $directory.FullName $name
                if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
            }
            if (!(Get-ChildItem -LiteralPath $directory.FullName -Force)) { Remove-Item -LiteralPath $directory.FullName }
        }
        foreach ($name in @('pending', 'pending.tmp', 'updates.log')) {
            $path = Join-Path $handoffRoot $name
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
        }
        if (!(Get-ChildItem -LiteralPath $handoffRoot -Force)) { Remove-Item -LiteralPath $handoffRoot }
        Write-Host "PASS: reader $mode with inherited shutdown signal."
    }

    $wrongFolder = Join-Path $folder 'wrong publisher'
    New-Item -ItemType Directory -Path $wrongFolder | Out-Null
    $wrongPayload = Join-Path $wrongFolder 'payload.exe'
    Copy-Item -LiteralPath (Join-Path $repo 'build\Release\update_fixture.exe') -Destination $wrongPayload
    $wrongCertificate = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=Voltura Update Test Only' -CertStoreLocation 'Cert:\CurrentUser\My' -KeyAlgorithm RSA -KeyLength 2048 -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddDays(2)
    try {
        & (Join-Path $PSScriptRoot 'sign.ps1') -Path $wrongPayload -CertificateThumbprint $wrongCertificate.Thumbprint -TimestampUrl $signing.timestampUrl -AllowSelfSigned
        & $runner --verify-candidate $runner $wrongPayload
        if ($LASTEXITCODE -eq 0) { throw 'Wrong publisher update accepted.' }
        Write-Host 'PASS: valid signature from another publisher rejected.'
    } finally { Remove-Item -LiteralPath "Cert:\CurrentUser\My\$($wrongCertificate.Thumbprint)" -DeleteKey }

    # A separately identified real NSIS package exercises installed updates without touching
    # the normal installation, shortcut, association, or preference file.
    $productKey = "VolturaMarkdown-Test-$identity"
    $shortcutName = "Voltura Markdown Update Test $identity"
    $installDir = Join-Path $folder 'installed copy'
    $installedTarget = Join-Path $installDir 'vmd.exe'
    New-Item -ItemType Directory -Path $installDir | Out-Null
    Copy-Item -LiteralPath $runner -Destination $installedTarget
    $registryPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$productKey"
    New-Item -Path $registryPath -Force | Out-Null
    Set-ItemProperty -LiteralPath $registryPath -Name InstallLocation -Value $installDir
    $setupPath = Join-Path $folder 'Setup fixture.exe'
    $compiler = Get-Command makensis.exe -ErrorAction SilentlyContinue
    if (!$compiler) { $compiler = Get-Command (Join-Path ${env:ProgramFiles(x86)} 'NSIS\makensis.exe') -ErrorAction Stop }
    try {
        & $compiler.Source /WX /V2 '/DVERSION=1.2.1' "/DOUTPUT=$setupPath" "/DPRODUCT_KEY=$productKey" "/DSHORTCUT_NAME=$shortcutName" "/DAPP_BINARY=$fixture" (Join-Path $PSScriptRoot 'installer.nsi')
        if ($LASTEXITCODE) { throw 'Isolated update installer build failed.' }
        & (Join-Path $PSScriptRoot 'sign.ps1') -Path $setupPath -CertificateThumbprint $signing.certificateThumbprint -TimestampUrl $signing.timestampUrl -AllowSelfSigned:([bool]$signing.allowSelfSigned)
        $oldTestKey = $env:VMD_UPDATES_TEST_PRODUCT_KEY
        $env:VMD_UPDATES_TEST_PRODUCT_KEY = $productKey
        try {
            $helper = Start-Helper $installedTarget $setupPath -Setup; $helpers += $helper
            Wait-Helper $helper $true
        } finally { $env:VMD_UPDATES_TEST_PRODUCT_KEY = $oldTestKey }
        if ((Get-FileHash -LiteralPath $installedTarget -Algorithm SHA256).Hash -ne $newHash) { throw 'Installed update differs from signed payload.' }
        if ((Get-ItemProperty -LiteralPath $registryPath).DisplayVersion -ne '1.2.1') { throw 'Installed update registration did not advance.' }
        Write-Host 'PASS: real signed silent NSIS update to existing installation.'
    } finally {
        if (Test-Path -LiteralPath (Join-Path $installDir 'Uninstall.exe')) {
            $uninstall = Start-Process -FilePath (Join-Path $installDir 'Uninstall.exe') -ArgumentList @('/S', "_?=$installDir") -WindowStyle Hidden -PassThru -Wait
            if ($uninstall.ExitCode) { throw 'Isolated update test uninstall failed.' }
        } elseif (Test-Path -LiteralPath $registryPath) {
            Remove-Item -LiteralPath $registryPath
        }
    }
} finally {
    $env:VMD_UPDATES_TEST_PROFILE = $oldTestProfile
    # Stop only test-owned hidden readers/helpers and remove only their isolated cache files.
    foreach ($process in @($readers) + @($helpers | ForEach-Object { $_.Process })) {
        if (!$process.HasExited) {
            $window = [UpdateTestWindows]::Find($process.Id)
            if ($window -ne [IntPtr]::Zero) { [void][UpdateTestWindows]::SendMessageW($window, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) }
            if (!$process.WaitForExit(3000)) { $process.Kill(); $process.WaitForExit() }
        }
    }
    foreach ($helper in $helpers) {
        $directory = Split-Path -Parent $helper.Path
        foreach ($name in @('payload.exe', 'helper.exe', 'download.part')) {
            $path = Join-Path $directory $name
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
        }
        if (!(Get-ChildItem -LiteralPath $directory -Force)) { Remove-Item -LiteralPath $directory }
        foreach ($name in @('pending', 'pending.tmp', 'updates.log')) {
            $path = Join-Path $helper.Root $name
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
        }
        if (!(Get-ChildItem -LiteralPath $helper.Root -Force)) { Remove-Item -LiteralPath $helper.Root }
    }
}
Write-Host "Signed fixtures and results retained in $folder"
