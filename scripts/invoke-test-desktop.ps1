function Invoke-IsolatedGuiTest {
    param([string]$Script, [hashtable]$Parameters)

    $arguments = @('-NoProfile', '-File', $Script, '-Interactive')
    foreach ($key in $Parameters.Keys) {
        if ($key -eq 'Interactive') { continue }
        $value = $Parameters[$key]
        if ($value -is [System.Management.Automation.SwitchParameter]) {
            if ($value.IsPresent) { $arguments += "-$key" }
        }
        else { $arguments += @("-$key", [string]$value) }
    }
    $shell = (Get-Process -Id $PID).Path
    & python (Join-Path $PSScriptRoot 'test_desktop.py') -- $shell @arguments
    if ($LASTEXITCODE) { throw "Isolated GUI checks failed (exit $LASTEXITCODE)." }
}
