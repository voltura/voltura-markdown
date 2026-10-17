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
function Invoke-Gh {
    param([string[]]$Arguments)
    & gh @Arguments
    if ($LASTEXITCODE) { throw "GitHub command failed: $($Arguments -join ' ')" }
}
function Invoke-JsonApi {
    param([string]$Method, [string]$Endpoint, [object]$Body)
    $requestFile = Join-Path $repo ('build/pages-request-' + [Guid]::NewGuid().ToString('N') + '.json')
    [IO.File]::WriteAllText($requestFile, ($Body | ConvertTo-Json -Depth 10), [Text.UTF8Encoding]::new($false))
    $response = Invoke-Gh -Arguments @('api', '--method', $Method, $Endpoint, '--input', $requestFile)
    return (($response | Out-String) | ConvertFrom-Json)
}

# Publish generated files through GitHub's Git database API; never create a local checkout or switch main.
Invoke-Gh -Arguments @('auth', 'status')
$gitApi = "repos/$Repository/git"
$refs = (Invoke-Gh -Arguments @('api', "$gitApi/matching-refs/heads/gh-pages") | Out-String) | ConvertFrom-Json
$currentRef = @($refs | Where-Object { $_.ref -eq 'refs/heads/gh-pages' })
$parent = if ($currentRef.Count) { $currentRef[0].object.sha } else { $null }
$entries = @()
$previousTree = $null
if ($parent) {
    $commit = (Invoke-Gh -Arguments @('api', "$gitApi/commits/$parent") | Out-String) | ConvertFrom-Json
    $previousTree = $commit.tree.sha
    $tree = (Invoke-Gh -Arguments @('api', "$gitApi/trees/$previousTree") | Out-String) | ConvertFrom-Json
    foreach ($entry in @($tree.tree | Where-Object { $_.path -eq 'CNAME' })) {
        $entries += @{ path = 'CNAME'; mode = $entry.mode; type = $entry.type; sha = $entry.sha }
    }
}
$built = Join-Path $repo 'build/pages'
foreach ($file in Get-ChildItem -LiteralPath $built -File -Recurse) {
    $relative = [IO.Path]::GetRelativePath($built, $file.FullName).Replace('\', '/')
    $blob = Invoke-JsonApi -Method POST -Endpoint "$gitApi/blobs" -Body @{
        encoding = 'base64'
        content = [Convert]::ToBase64String([IO.File]::ReadAllBytes($file.FullName))
    }
    $entries += @{ path = $relative; mode = '100644'; type = 'blob'; sha = $blob.sha }
}
$newTree = Invoke-JsonApi -Method POST -Endpoint "$gitApi/trees" -Body @{ tree = @($entries) }
if ($newTree.sha -ne $previousTree) {
    $body = @{ message = 'Publish Voltura Markdown product site'; tree = $newTree.sha; parents = @() }
    if ($parent) { $body.parents = @($parent) }
    $commit = Invoke-JsonApi -Method POST -Endpoint "$gitApi/commits" -Body $body
    if ($parent) {
        Invoke-JsonApi -Method PATCH -Endpoint "$gitApi/refs/heads/gh-pages" -Body @{ sha = $commit.sha; force = $false } | Out-Null
    } else {
        Invoke-JsonApi -Method POST -Endpoint "$gitApi/refs" -Body @{ ref = 'refs/heads/gh-pages'; sha = $commit.sha } | Out-Null
    }
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
Write-Host 'Generated gh-pages files published and branch-based GitHub Pages configured.'
Write-Host "Publication status: https://github.com/$Repository/settings/pages"
Write-Host 'Primary source checkout remains on main; no local Pages checkout was created.'
