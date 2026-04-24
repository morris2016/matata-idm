# pack-extension.ps1
#
# Produces build\matata-extension-<version>.zip — the exact bundle you
# upload to the Chrome Web Store developer dashboard.
#
# The script:
#   1. validates manifest.json (presence, JSON parse, required fields)
#   2. verifies all referenced icon files exist
#   3. copies extension\ into a staging dir, stripping editor junk
#      (.DS_Store, Thumbs.db, *.swp, *.bak)
#   4. zips it and prints the resulting path + size
#
# Usage:  powershell -ExecutionPolicy Bypass -File pack-extension.ps1

$ErrorActionPreference = "Stop"

$root   = $PSScriptRoot
$extDir = Join-Path $root "extension"
$outDir = Join-Path $root "build"
$manifest = Join-Path $extDir "manifest.json"

if (-not (Test-Path $manifest)) {
    Write-Error "manifest.json not found at $manifest"
    exit 1
}

$m = Get-Content $manifest -Raw | ConvertFrom-Json
if (-not $m.name)            { Write-Error "manifest.json: missing 'name'";            exit 1 }
if (-not $m.version)         { Write-Error "manifest.json: missing 'version'";         exit 1 }
if (-not $m.manifest_version){ Write-Error "manifest.json: missing 'manifest_version'";exit 1 }
if ($m.manifest_version -ne 3) {
    Write-Error "Chrome Web Store only accepts MV3. manifest_version=$($m.manifest_version)"
    exit 1
}

# Verify every icon file the manifest references.
$iconPaths = @()
if ($m.icons)                 { $iconPaths += $m.icons.PSObject.Properties.Value }
if ($m.action -and $m.action.default_icon) {
    $iconPaths += $m.action.default_icon.PSObject.Properties.Value
}
$iconPaths | Sort-Object -Unique | ForEach-Object {
    $p = Join-Path $extDir $_
    if (-not (Test-Path $p)) {
        Write-Error "manifest references missing icon: $_"
        exit 1
    }
}

# Stage a clean copy (skip editor/VCS junk).
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
$stage = Join-Path $env:TEMP ("matata-ext-" + [guid]::NewGuid().ToString())
$null  = New-Item -ItemType Directory -Path $stage

$excludePatterns = @("*.swp", "*.bak", "Thumbs.db", ".DS_Store", ".git", ".gitignore")
Get-ChildItem $extDir -Recurse -File | ForEach-Object {
    $skip = $false
    foreach ($pat in $excludePatterns) { if ($_.Name -like $pat) { $skip = $true; break } }
    if ($skip) { return }
    $rel     = $_.FullName.Substring($extDir.Length + 1)
    $target  = Join-Path $stage $rel
    $targetDir = Split-Path $target -Parent
    if (-not (Test-Path $targetDir)) { New-Item -ItemType Directory -Path $targetDir -Force | Out-Null }
    Copy-Item $_.FullName $target
}

$zipName = "matata-extension-$($m.version).zip"
$zipPath = Join-Path $outDir $zipName
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }

# Compress-Archive is fine — the Web Store accepts standard zip.
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zipPath -Force

Remove-Item $stage -Recurse -Force

$size = (Get-Item $zipPath).Length
Write-Host ""
Write-Host "[ok] $zipPath"
Write-Host "     $size bytes  ($([math]::Round($size/1kb,1)) KB)"
Write-Host ""
Write-Host "Upload this file at https://chrome.google.com/webstore/devconsole"
Write-Host "(You still need the native host installed separately via install-extension.bat"
Write-Host " using the Web-Store extension ID -- see README.)"
