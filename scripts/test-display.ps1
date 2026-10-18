param(
    [string]$Document = 'tests/showcase.md',
    [switch]$Scroll,
    [switch]$Animate,
    [switch]$TraceDisplay,
    [switch]$CloseFullscreen,
    [switch]$StagedRenderer,
    [switch]$Interactive,
    [string]$PresentMon = 'build/tools/PresentMon.exe'
)

$ErrorActionPreference = 'Stop'
if (!$Interactive) {
    if ($TraceDisplay) { throw 'Display tracing needs the visible desktop; use -Interactive -TraceDisplay.' }
    . (Join-Path $PSScriptRoot 'invoke-test-desktop.ps1')
    Invoke-IsolatedGuiTest -Script $PSCommandPath -Parameters $PSBoundParameters
    return
}
$repo = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $repo 'build/Release/vmd-test.exe'
$documentPath = (Resolve-Path -LiteralPath $Document).Path
$output = Join-Path $repo ('build/display-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output | Out-Null
Get-CimInstance Win32_VideoController |
    Select-Object Name, CurrentHorizontalResolution, CurrentVerticalResolution, CurrentRefreshRate |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'display.json') -Encoding utf8
$log = Join-Path $output 'checks.jsonl'
$metrics = Join-Path $output 'startup.jsonl'
$trace = $null
$app = $null
try {
    if ($TraceDisplay) {
        $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
        $principal = [Security.Principal.WindowsPrincipal]::new($identity)
        if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
            throw 'Display tracing requires an elevated PowerShell session. Run without -TraceDisplay for DPI and CPU paint checks.'
        }
        $presentMonPath = (Resolve-Path -LiteralPath $PresentMon).Path
        $trace = Start-Process -FilePath $presentMonPath -ArgumentList @(
            '--process_name', 'vmd-test.exe', '--session_name', ('Voltura-' + [Guid]::NewGuid().ToString('N')),
            '--timed', '40', '--terminate_after_timed', '--no_console_stats',
            '--output_file', ('"' + (Join-Path $output 'frames.csv') + '"')
        ) -PassThru -WindowStyle Hidden
        Start-Sleep -Milliseconds 500
        if ($trace.HasExited) { throw 'PresentMon could not start its trace session.' }
    }
    $mode = if ($Scroll) { '--scroll-benchmark' } else { '--verify-ui' }
    $appArguments = @(
        ('"' + $documentPath + '"'), $mode, ('"' + $log + '"'),
        '--metrics', ('"' + $metrics + '"'), '--no-preferences', '--exit-after', '35000'
    )
    if ($CloseFullscreen) { $appArguments += '--verify-close-fullscreen' }
    if ($Animate) { $appArguments += '--benchmark-animations' }
    if ($StagedRenderer) { $appArguments += '--staged-renderer' }
    $app = Start-Process -FilePath $executable -ArgumentList $appArguments -PassThru -WindowStyle Normal
    if (!$app.WaitForExit(40000)) { throw 'Display test timed out.' }
    if ($app.ExitCode) { throw 'Display test process failed.' }
    if (!(Test-Path -LiteralPath $log)) { throw 'No display test results were written.' }
    $results = @(Get-Content -LiteralPath $log | ForEach-Object { $_ | ConvertFrom-Json })
    if (@($results | Where-Object { $_.passed -eq $false }).Count) { throw "Display checks failed: $log" }
    if ($Scroll) {
        if ($results[-1].passed -ne $true -or $results[-1].scroll_input_samples -lt 1 -or $results[-1].scroll_paint_samples -lt 1) {
            throw 'Scroll test did not render document frames.'
        }
    }
    else {
        $expected = if ($CloseFullscreen) { 'fullscreen_x_closes_process_without_restoring_window' } else { 'escape_closes_normal_window' }
        if ($results[-1].check -ne $expected -or !$results[-1].passed) { throw 'UI test did not finish.' }
    }
    $results | Where-Object { $_.scroll_paint_samples -gt 0 } | ConvertTo-Json -Compress
    if ($trace) {
        if (!$trace.WaitForExit(45000)) { throw 'Display trace did not finish.' }
        if ($trace.ExitCode) { throw 'Display trace failed.' }
        $framesPath = Join-Path $output 'frames.csv'
        $frames = @(Import-Csv -LiteralPath $framesPath | Where-Object { $_.ProcessID -eq $app.Id })
        if (!$frames.Count) { throw 'No display frames were captured for the test-owned process.' }
        $frames | Export-Csv -LiteralPath $framesPath -NoTypeInformation
    }
    Write-Host "Results: $output"
}
finally {
    foreach ($ownedProcess in @($app, $trace)) {
        if ($ownedProcess -and !$ownedProcess.HasExited) { Stop-Process -Id $ownedProcess.Id }
    }
}
