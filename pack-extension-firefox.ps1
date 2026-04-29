# pack-extension-firefox.ps1
#
# Produces build\matata-extension-firefox-<version>.zip -- the bundle you
# upload to addons.mozilla.org (AMO).
#
# Differences from pack-extension.ps1 (Chrome target):
#   1. strips Chrome-only "minimum_chrome_version" from the manifest so AMO
#      doesn't flag it as an unknown field during validation
#   2. asserts "browser_specific_settings.gecko" is present (AMO needs the
#      add-on ID + minimum Firefox version)
#   3. asserts background.scripts is present (Firefox MV3 uses scripts; the
#      service_worker key alone won't run)
#   4. names the output -firefox-<version>.zip
#
# Usage:  powershell -ExecutionPolicy Bypass -File pack-extension-firefox.ps1

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
if (-not $m.name)             { Write-Error "manifest.json: missing 'name'";             exit 1 }
if (-not $m.version)          { Write-Error "manifest.json: missing 'version'";          exit 1 }
if (-not $m.manifest_version) { Write-Error "manifest.json: missing 'manifest_version'"; exit 1 }
if ($m.manifest_version -ne 3) {
    Write-Error "AMO accepts MV3. manifest_version=$($m.manifest_version)"
    exit 1
}
if (-not $m.browser_specific_settings -or -not $m.browser_specific_settings.gecko) {
    Write-Error "manifest.json: missing browser_specific_settings.gecko (required by AMO)"
    exit 1
}
if (-not $m.browser_specific_settings.gecko.id) {
    Write-Error "manifest.json: browser_specific_settings.gecko.id is required"
    exit 1
}
if (-not $m.background -or -not $m.background.scripts) {
    Write-Error "manifest.json: background.scripts missing (Firefox MV3 needs it)"
    exit 1
}

# Verify every icon file the manifest references.
$iconPaths = @()
if ($m.icons) { $iconPaths += $m.icons.PSObject.Properties.Value }
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

# Stage a clean copy (skip editor/VCS junk + Chrome store listing files).
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
$stage = Join-Path $env:TEMP ("matata-ext-fx-" + [guid]::NewGuid().ToString())
$null  = New-Item -ItemType Directory -Path $stage

$excludePatterns = @(
    "*.swp", "*.bak", "Thumbs.db", ".DS_Store", ".git", ".gitignore",
    "STORE_LISTING.md", "LISTING_DESCRIPTION.md", "PRIVACY_TAB.md",
    # The signed XPI is shipped by the matata installer, not packaged
    # inside an AMO submission -- including it would self-nest and bloat
    # the upload.
    "*.xpi"
)
Get-ChildItem $extDir -Recurse -File | ForEach-Object {
    $skip = $false
    foreach ($pat in $excludePatterns) { if ($_.Name -like $pat) { $skip = $true; break } }
    if ($skip) { return }
    $rel       = $_.FullName.Substring($extDir.Length + 1)
    $target    = Join-Path $stage $rel
    $targetDir = Split-Path $target -Parent
    if (-not (Test-Path $targetDir)) { New-Item -ItemType Directory -Path $targetDir -Force | Out-Null }
    Copy-Item $_.FullName $target
}

# Rewrite the staged manifest.json without the Chrome-only key. AMO's
# validator (addons-linter) flags unknown manifest properties, and
# "minimum_chrome_version" is meaningless on Firefox -- gecko uses
# browser_specific_settings.gecko.strict_min_version instead.
#
# Regex-strip the line instead of round-tripping ConvertFrom-Json /
# ConvertTo-Json -- that pipeline mangles non-ASCII (em-dashes in the
# description) and escapes <all_urls> as <all_urls>.
$stagedManifest = Join-Path $stage "manifest.json"
$utf8NoBom = New-Object System.Text.UTF8Encoding $false
$text = [System.IO.File]::ReadAllText($stagedManifest, $utf8NoBom)
$text = [regex]::Replace($text,
    '(?m)^[ \t]*"minimum_chrome_version"[ \t]*:[ \t]*"[^"]*"[ \t]*,?[ \t]*\r?\n', '')
# "background.service_worker" is unsupported on Firefox -- the validator
# warns. Strip the line; Firefox uses background.scripts.
$text = [regex]::Replace($text,
    '(?m)^[ \t]*"service_worker"[ \t]*:[ \t]*"[^"]*"[ \t]*,?[ \t]*\r?\n', '')
[System.IO.File]::WriteAllText($stagedManifest, $text, $utf8NoBom)

$zipName = "matata-extension-firefox-$($m.version).zip"
$zipPath = Join-Path $outDir $zipName
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }

# Build the archive entry-by-entry with forward-slash names. Both
# Compress-Archive and [ZipFile]::CreateFromDirectory on .NET Framework 4.x
# (PS 5.1's runtime) write backslashes as path separators, which violates
# the ZIP spec. AMO's validator rejects this with "Invalid file name in
# archive: icons\icon-128.png".
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$fs = [System.IO.File]::Open($zipPath, [System.IO.FileMode]::CreateNew)
try {
    $zip = New-Object System.IO.Compression.ZipArchive(
        $fs, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        $stageRoot = (Resolve-Path $stage).Path
        Get-ChildItem $stage -Recurse -File | ForEach-Object {
            $rel  = $_.FullName.Substring($stageRoot.Length).TrimStart('\','/')
            $name = $rel -replace '\\','/'
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zip, $_.FullName, $name,
                [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally { $zip.Dispose() }
} finally { $fs.Dispose() }
Remove-Item $stage -Recurse -Force

$size = (Get-Item $zipPath).Length
Write-Host ""
Write-Host "[ok] $zipPath"
Write-Host "     $size bytes  ($([math]::Round($size/1kb,1)) KB)"
Write-Host ""
Write-Host "Upload at https://addons.mozilla.org/developers/addon/submit/upload-listed"
Write-Host ""
Write-Host "AMO review notes to include:"
Write-Host "  - matata is a desktop download manager. The extension forwards"
Write-Host "    download URLs + cookies to a separate native host process"
Write-Host "    (matata-host.exe) installed via install-extension.bat."
Write-Host "  - native messaging host name: com.matata.host"
Write-Host "  - source: same files as in the zip -- no build step, no minification."
Write-Host "  - test build of the native host: see README.md (build.bat)."
