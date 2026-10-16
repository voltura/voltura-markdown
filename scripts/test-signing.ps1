param(
    [string]$Path = (Join-Path $PSScriptRoot '..\build\Release\vmd.exe'),
    [string]$SigningConfig = (Join-Path $env:LOCALAPPDATA 'Voltura\Markdown\Signing\signing.json')
)

$ErrorActionPreference = 'Stop'
$configuration = Get-Content -LiteralPath $SigningConfig -Raw | ConvertFrom-Json
$directory = Join-Path (Split-Path -Parent $PSScriptRoot) ('build\signing-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $directory | Out-Null
$signed = Join-Path $directory 'signed.exe'
$tampered = Join-Path $directory 'tampered.exe'
Copy-Item -LiteralPath $Path -Destination $signed
$parameters = @{
    CertificateThumbprint = $configuration.certificateThumbprint
    AllowSelfSigned = [bool]$configuration.allowSelfSigned
}
& (Join-Path $PSScriptRoot 'sign.ps1') -Path $signed -TimestampUrl $configuration.timestampUrl @parameters
$verified = & (Join-Path $PSScriptRoot 'verify-signature.ps1') -Path $signed @parameters
if (!$verified.digestVerified -or !$verified.timestamped) { throw 'Signed fixture failed verification.' }

$bytes = [IO.File]::ReadAllBytes($signed)
$pe = [BitConverter]::ToInt32($bytes, 0x3c)
$section = $pe + 24 + [BitConverter]::ToUInt16($bytes, $pe + 20)
$raw = [BitConverter]::ToInt32($bytes, $section + 20)
$bytes[$raw + 64] = $bytes[$raw + 64] -bxor 1
[IO.File]::WriteAllBytes($tampered, $bytes)
try {
    & (Join-Path $PSScriptRoot 'verify-signature.ps1') -Path $tampered @parameters | Out-Null
    throw 'Modified executable was accepted.'
} catch {
    if ($_.Exception.Message -notlike '*0x80096010*') { throw }
}
if ($parameters.AllowSelfSigned) {
    try {
        & (Join-Path $PSScriptRoot 'verify-signature.ps1') -Path $signed -CertificateThumbprint $parameters.CertificateThumbprint | Out-Null
        throw 'Self-signed certificate was accepted without explicit permission.'
    } catch {
        if ($_.Exception.Message -notlike '*0x800B0109*') { throw }
    }
}
try {
    & (Join-Path $PSScriptRoot 'verify-signature.ps1') -Path $signed -CertificateThumbprint ('0' * 40) -AllowSelfSigned | Out-Null
    throw 'Wrong signer was accepted.'
} catch {
    if ($_.Exception.Message -notlike '*not signed by the selected certificate*') { throw }
}
Write-Host 'PASS: timestamped signature, modified-file rejection, signer pinning, and self-signed opt-in.'
