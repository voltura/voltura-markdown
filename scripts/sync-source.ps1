param([switch]$CheckOnly)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
function Read-Git {
    param([string[]]$Arguments)
    $result = & git -C $repo @Arguments
    if ($LASTEXITCODE) { throw "Git command failed: $($Arguments -join ' ')" }
    return ($result | Out-String).Trim()
}

$branch = Read-Git -Arguments @('branch', '--show-current')
if ($branch -ne 'main') { throw 'Use the primary checkout on main; no worktrees or other branches.' }
$gitDirectory = Read-Git -Arguments @('rev-parse', '--absolute-git-dir')
$commonDirectory = Read-Git -Arguments @('rev-parse', '--path-format=absolute', '--git-common-dir')
if ($gitDirectory -ne $commonDirectory) { throw 'Use the primary checkout, not a linked worktree.' }
Read-Git -Arguments @('fetch', 'origin', 'main') | Out-Null
$head = Read-Git -Arguments @('rev-parse', 'HEAD')
$remote = Read-Git -Arguments @('rev-parse', 'origin/main')
if ($head -eq $remote) {
    Write-Host "Source synchronized on main: $head"
    return
}
if ($CheckOnly) { throw 'Local main differs from origin/main. Reconcile and push reviewed changes before releasing.' }
$changes = Read-Git -Arguments @('status', '--porcelain')
if ($changes) { throw 'Sources differ and local edits exist. Preserve and reconcile them before editing further.' }
& git -C $repo merge-base --is-ancestor HEAD origin/main
if ($LASTEXITCODE -ne 0) { throw 'Local main has unpushed or divergent commits. Preserve and reconcile them; do not reset.' }
Read-Git -Arguments @('merge', '--ff-only', 'origin/main') | Write-Host
$head = Read-Git -Arguments @('rev-parse', 'HEAD')
if ($head -ne $remote) { throw 'Source synchronization did not reach origin/main.' }
Write-Host "Source synchronized on main: $head"
