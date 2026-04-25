# fetch-yt-dlp.ps1
#
# Pulls the latest yt-dlp.exe (single-file Python build) from GitHub.
# Idempotent — re-runs only re-fetch if forced or file missing.

param([switch]$Force)

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$out  = Join-Path $root "third-party\yt-dlp.exe"
$url  = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe"

if ((Test-Path $out) -and (-not $Force)) {
    $size = (Get-Item $out).Length
    if ($size -gt 5000000) {
        Write-Host "[yt-dlp] already present at $out ($size bytes)"
        exit 0
    }
}

New-Item -ItemType Directory -Path (Split-Path $out -Parent) -Force | Out-Null
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Write-Host "[yt-dlp] downloading $url"
Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing
$size = (Get-Item $out).Length
Write-Host "[yt-dlp] saved: $out ($size bytes)"
