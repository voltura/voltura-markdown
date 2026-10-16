param(
    [string]$Subject = 'CN=Voltura AB, O=Voltura AB, C=SE',
    [uri]$TimestampUrl = 'http://timestamp.digicert.com'
)

$ErrorActionPreference = 'Stop'
$directory = Join-Path $env:LOCALAPPDATA 'Voltura\Markdown\Signing'
$configurationPath = Join-Path $directory 'signing.json'
if (Test-Path -LiteralPath $configurationPath) {
    $configuration = Get-Content -LiteralPath $configurationPath -Raw | ConvertFrom-Json
    $certificate = Get-Item -LiteralPath "Cert:\CurrentUser\My\$($configuration.certificateThumbprint)" -ErrorAction Stop
    if (!$certificate.HasPrivateKey -or $certificate.NotAfter -le (Get-Date)) {
        throw 'The configured signing certificate is expired or its private key is unavailable.'
    }
    Write-Host "Reusing signing certificate: $($certificate.Thumbprint)"
    return
}
if ($TimestampUrl.Scheme -notin @('http', 'https') -or !$TimestampUrl.Host -or $TimestampUrl.UserInfo) {
    throw 'Provide an HTTP(S) RFC 3161 timestamp URL without credentials.'
}
$certificate = New-SelfSignedCertificate -Type CodeSigningCert -Subject $Subject `
    -FriendlyName 'Voltura Markdown local code signing' -CertStoreLocation 'Cert:\CurrentUser\My' `
    -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -KeyUsage DigitalSignature `
    -KeyExportPolicy NonExportable -Provider 'Microsoft Software Key Storage Provider' `
    -NotAfter (Get-Date).AddYears(2)
New-Item -ItemType Directory -Path $directory -Force | Out-Null
Export-Certificate -Cert $certificate -FilePath (Join-Path $directory 'Voltura-Markdown.cer') | Out-Null
[ordered]@{
    certificateThumbprint = $certificate.Thumbprint
    timestampUrl = $TimestampUrl.AbsoluteUri
    allowSelfSigned = $true
} | ConvertTo-Json | Set-Content -LiteralPath $configurationPath -Encoding utf8
Write-Host "Created signing certificate: $($certificate.Thumbprint)"
Write-Host "Local configuration: $configurationPath"
Write-Host 'Private key: non-exportable, CurrentUser Personal certificate store. No trust stores changed.'
