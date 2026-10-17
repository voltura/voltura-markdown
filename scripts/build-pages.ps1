param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9-]{0,38}/[A-Za-z0-9_.-]{1,100}$')]
    [string]$Repository,
    [ValidatePattern('^(?:|[A-Za-z0-9][A-Za-z0-9._/-]{0,127})$')]
    [string]$ReleaseTag = ''
)

$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$project = Get-Content -LiteralPath (Join-Path $repo 'CMakeLists.txt') -Raw
if ($project -notmatch 'project\(VolturaMarkdown VERSION (\d+\.\d+\.\d+)') {
    throw 'Project version missing.'
}
$releaseVersion = $Matches[1]
if ($ReleaseTag) {
    if ($ReleaseTag -notmatch '^v(\d+\.\d+\.\d+)(?:-[A-Za-z0-9.-]+)?$') {
        throw 'ReleaseTag must identify an asset version, for example v1.1.4 or v1.1.4-preview.1.'
    }
    $releaseVersion = $Matches[1]
}
$repositoryName = $Repository.Split('/')[1]
if ($repositoryName -eq '.' -or $repositoryName -eq '..') {
    throw 'Use an actual owner/repository name.'
}
$output = [IO.Path]::GetFullPath((Join-Path $repo 'build\pages'))
$expected = [IO.Path]::GetFullPath((Join-Path $repo 'build\pages'))
if ($output -ne $expected -or !($output.StartsWith($repo + [IO.Path]::DirectorySeparatorChar))) {
    throw 'Pages output must stay inside the repository build directory.'
}
$buildDirectory = Join-Path $repo 'build'
if ((Test-Path -LiteralPath $buildDirectory) -and ((Get-Item -LiteralPath $buildDirectory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw 'The build directory must not be a junction or symbolic link.'
}
if (Test-Path -LiteralPath $output) {
    $item = Get-Item -LiteralPath $output -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Pages output must not be a junction or symbolic link.'
    }
    Remove-Item -LiteralPath $output -Recurse -Force
}
New-Item -ItemType Directory -Path (Join-Path $output 'assets') -Force | Out-Null
$repositoryUrl = "https://github.com/$Repository"
if ($ReleaseTag.Contains('..')) { throw 'Release tags must not contain consecutive dots.' }
$downloadUrl = if ($ReleaseTag) {
    "$repositoryUrl/releases/download/$([uri]::EscapeDataString($ReleaseTag))"
} else {
    "$repositoryUrl/releases/download/v$releaseVersion"
}
$page = Get-Content -LiteralPath (Join-Path $repo 'site\index.html') -Raw
$page = $page.Replace('__REPOSITORY_URL__', $repositoryUrl).Replace('__RELEASES_URL__', "$repositoryUrl/releases").Replace('__VERSION__', $releaseVersion).Replace('__DOWNLOAD_URL__', $downloadUrl)
if ($page -match '__[A-Z_]+__') {
    throw 'An unresolved Pages template token remains.'
}
[IO.File]::WriteAllText((Join-Path $output 'index.html'), $page, [Text.UTF8Encoding]::new($false))
Copy-Item -LiteralPath (Join-Path $repo 'site\styles.css') -Destination (Join-Path $output 'styles.css')
Copy-Item -LiteralPath (Join-Path $repo 'assets\icon-master.png') -Destination (Join-Path $output 'assets\icon.png')
Copy-Item -LiteralPath (Join-Path $repo 'site\assets\social-preview.png') -Destination (Join-Path $output 'assets\social-preview.png')
[IO.File]::WriteAllText((Join-Path $output '.nojekyll'), '')
Write-Host "Pages built: $output"
Write-Host "Repository links: $repositoryUrl"
Write-Host 'Preview: python -m http.server 8080 --bind 127.0.0.1 --directory build/pages'
