# fetch-webview2-runtime.ps1
#
# Downloads MicrosoftEdgeWebview2Setup.exe (the evergreen bootstrapper, ~1.8 MB)
# into third-party/. The installer bundles this so end users without the
# WebView2 runtime get it installed automatically.
#
# Usage: powershell -ExecutionPolicy Bypass -File fetch-webview2-runtime.ps1

$ErrorActionPreference = "Stop"

$root  = $PSScriptRoot
$out   = Join-Path $root "third-party\MicrosoftEdgeWebview2Setup.exe"
$url   = "https://go.microsoft.com/fwlink/p/?LinkId=2124703"

if (Test-Path $out) {
    $size = (Get-Item $out).Length
    if ($size -gt 1000000) {
        Write-Host "[webview2-rt] already present at $out ($size bytes)"
        exit 0
    }
}

New-Item -ItemType Directory -Path (Split-Path $out -Parent) -Force | Out-Null

Write-Host "[webview2-rt] downloading $url"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing

$size = (Get-Item $out).Length
Write-Host "[webview2-rt] saved: $out ($size bytes)"
