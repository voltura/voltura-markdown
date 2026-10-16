param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9-]{0,38}/[A-Za-z0-9_.-]{1,100}$')]
    [string]$Repository,
    [ValidatePattern('^(?:|[A-Za-z0-9][A-Za-z0-9._/-]{0,127})$')]
    [string]$ReleaseTag = '',
    [switch]$Publish
)

$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
& (Join-Path $PSScriptRoot 'build-pages.ps1') -Repository $Repository -ReleaseTag $ReleaseTag
if (!$Publish) {
    Write-Host 'Local build only. Add -Publish to push gh-pages and configure GitHub Pages.'
    return
}

foreach ($tool in @('git', 'gh')) {
    if (!(Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool is required for publication."
    }
}
function Invoke-Git {
    param([string[]]$Arguments)
    & git @Arguments
    if ($LASTEXITCODE) { throw "Git command failed: $($Arguments -join ' ')" }
}
function Invoke-Gh {
    param([string[]]$Arguments)
    & gh @Arguments
    if ($LASTEXITCODE) { throw "GitHub command failed: $($Arguments -join ' ')" }
}

# Only explicit -Publish reaches authenticated network operations.
Invoke-Gh -Arguments @('auth', 'status')
$remoteUrl = "https://github.com/$Repository.git"
$checkout = Join-Path $repo ('build\pages-publish\' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $checkout -Force | Out-Null
Invoke-Git -Arguments @('init', $checkout)
Invoke-Git -Arguments @('-C', $checkout, 'var', 'GIT_AUTHOR_IDENT') | Out-Null
Invoke-Git -Arguments @('-C', $checkout, 'remote', 'add', 'origin', $remoteUrl)
$customDomain = $null
$existing = @(Invoke-Git -Arguments @('-C', $checkout, 'ls-remote', '--heads', 'origin', 'refs/heads/gh-pages'))
if ($existing.Count -gt 0) {
    Invoke-Git -Arguments @('-C', $checkout, 'fetch', '--depth', '1', 'origin', 'gh-pages')
    Invoke-Git -Arguments @('-C', $checkout, 'checkout', '-b', 'gh-pages', 'FETCH_HEAD')
    $domainFile = Join-Path $checkout 'CNAME'
    $customDomain = if (Test-Path -LiteralPath $domainFile) { [IO.File]::ReadAllText($domainFile) } else { $null }
    # Work only in this fresh generated checkout. Remove tracked page content while preserving .git.
    Invoke-Git -Arguments @('-C', $checkout, 'rm', '-r', '--ignore-unmatch', '--', '.')
} else {
    Invoke-Git -Arguments @('-C', $checkout, 'checkout', '--orphan', 'gh-pages')
}
$built = Join-Path $repo 'build\pages'
Get-ChildItem -LiteralPath $built -Force | Copy-Item -Destination $checkout -Recurse -Force
if ($customDomain) {
    [IO.File]::WriteAllText((Join-Path $checkout 'CNAME'), $customDomain, [Text.UTF8Encoding]::new($false))
}
Invoke-Git -Arguments @('-C', $checkout, 'add', '--all')
& git -C $checkout diff --cached --quiet
if ($LASTEXITCODE -eq 1) {
    Invoke-Git -Arguments @('-C', $checkout, 'commit', '-m', 'Publish Voltura Markdown product site')
    Invoke-Git -Arguments @('-C', $checkout, 'push', 'origin', 'HEAD:refs/heads/gh-pages')
} elseif ($LASTEXITCODE -ne 0) {
    throw 'Could not inspect the staged Pages changes.'
} else {
    Write-Host 'Published page content is already current.'
}

$pagesApi = "repos/$Repository/pages"
$settingsFile = Join-Path $repo ('build\pages-settings-' + [Guid]::NewGuid().ToString('N') + '.json')
[IO.File]::WriteAllText($settingsFile, '{"build_type":"legacy","source":{"branch":"gh-pages","path":"/"}}', [Text.UTF8Encoding]::new($false))
# Confirm whether a Pages site exists without treating an authentication error as absence.
$repositoryInfo = Invoke-Gh -Arguments @('api', "repos/$Repository", '--jq', '.has_pages')
if (($repositoryInfo | Out-String).Trim() -eq 'true') {
    Invoke-Gh -Arguments @('api', '--method', 'PUT', $pagesApi, '--input', $settingsFile)
} else {
    try {
        Invoke-Gh -Arguments @('api', '--method', 'POST', $pagesApi, '--input', $settingsFile)
    } catch {
        # Pushing the first gh-pages branch can enable Pages before has_pages catches up.
        $creationError = $_
        $existingPages = & gh api $pagesApi 2>$null
        if ($LASTEXITCODE) { throw $creationError }
        $pages = ($existingPages | Out-String) | ConvertFrom-Json
        if ($pages.build_type -ne 'legacy' -or $pages.source.branch -ne 'gh-pages' -or $pages.source.path -ne '/') {
            Invoke-Gh -Arguments @('api', '--method', 'PUT', $pagesApi, '--input', $settingsFile)
        }
    }
}
Write-Host 'gh-pages pushed and branch-based GitHub Pages configured.'
Write-Host "Publication status: https://github.com/$Repository/settings/pages"
Write-Host "Isolated checkout retained for inspection: $checkout"
