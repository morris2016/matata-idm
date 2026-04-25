# fetch-ffmpeg.ps1
#
# Pulls a static x64 ffmpeg.exe from BtbN's release builds. yt-dlp uses
# this to merge separate video+audio streams into proper MP4 containers
# (otherwise it falls back to WebM, which surprises users).
#
# Idempotent: re-running re-fetches only with -Force.

param([switch]$Force)

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$out  = Join-Path $root "third-party\ffmpeg.exe"
$url  = "https://github.com/BtbN/FFmpeg-Builds/releases/latest/download/ffmpeg-master-latest-win64-lgpl.zip"

if ((Test-Path $out) -and (-not $Force)) {
    $size = (Get-Item $out).Length
    if ($size -gt 50000000) {
        Write-Host "[ffmpeg] already present at $out ($size bytes)"
        exit 0
    }
}

New-Item -ItemType Directory -Path (Split-Path $out -Parent) -Force | Out-Null

$zip = Join-Path $env:TEMP ("ffmpeg-" + [guid]::NewGuid().ToString() + ".zip")
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Write-Host "[ffmpeg] downloading $url (~80 MB) ..."
Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing

$stage = Join-Path $env:TEMP ("ffmpeg-stage-" + [guid]::NewGuid().ToString())
Expand-Archive -LiteralPath $zip -DestinationPath $stage -Force

# Layout: <stage>/ffmpeg-master-latest-win64-lgpl/bin/ffmpeg.exe
$found = Get-ChildItem -Path $stage -Recurse -Filter "ffmpeg.exe" | Select-Object -First 1
if (-not $found) { throw "[ffmpeg] ffmpeg.exe not found in $zip" }
Copy-Item $found.FullName $out -Force

Remove-Item $stage -Recurse -Force
Remove-Item $zip -Force

$size = (Get-Item $out).Length
Write-Host "[ffmpeg] saved: $out ($size bytes)"
