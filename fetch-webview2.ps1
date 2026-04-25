# fetch-webview2.ps1
#
# Downloads the Microsoft.Web.WebView2 NuGet package (it's just a zip) and
# extracts WebView2.h + WebView2EnvironmentOptions.h + WebView2LoaderStatic.lib
# into third-party/webview2/. Idempotent: no-op if already present.
#
# Usage: powershell -ExecutionPolicy Bypass -File fetch-webview2.ps1 [-Version 1.0.2903.40]

param(
    [string]$Version = "1.0.2903.40"
)

$ErrorActionPreference = "Stop"

$root      = $PSScriptRoot
$outDir    = Join-Path $root "third-party\webview2"
$includeDir = Join-Path $outDir "include"
$libDir     = Join-Path $outDir "lib\x64"
$stampFile  = Join-Path $outDir ".version"

# Skip if already fetched at the requested version.
if ((Test-Path $stampFile) -and ((Get-Content $stampFile -Raw).Trim() -eq $Version)) {
    Write-Host "[webview2] $Version already installed at $outDir"
    exit 0
}

if (Test-Path $outDir) { Remove-Item $outDir -Recurse -Force }
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$nupkg = Join-Path $env:TEMP ("webview2-" + $Version + ".zip")
$url   = "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/$Version"

Write-Host "[webview2] downloading $url"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Invoke-WebRequest -Uri $url -OutFile $nupkg -UseBasicParsing

$stage = Join-Path $env:TEMP ("webview2-stage-" + [guid]::NewGuid().ToString())
Expand-Archive -LiteralPath $nupkg -DestinationPath $stage -Force

# The nupkg layout is build/native/include/*.h and build/native/x64/*.lib
$srcInclude = Join-Path $stage "build\native\include"
$srcLibX64  = Join-Path $stage "build\native\x64"

if (-not (Test-Path $srcInclude)) { throw "[webview2] include dir missing in package: $srcInclude" }
if (-not (Test-Path $srcLibX64))  { throw "[webview2] x64 lib dir missing: $srcLibX64" }

New-Item -ItemType Directory -Path $includeDir -Force | Out-Null
New-Item -ItemType Directory -Path $libDir     -Force | Out-Null

Copy-Item (Join-Path $srcInclude "*.h")   $includeDir -Force
Copy-Item (Join-Path $srcLibX64  "*.lib") $libDir     -Force

# Drop a version stamp for the idempotency check.
Set-Content -Path $stampFile -Value $Version -Encoding ascii

Remove-Item $stage -Recurse -Force
Remove-Item $nupkg -Force

Write-Host ""
Write-Host "[webview2] installed ${Version}:"
Write-Host "  include: $includeDir"
Write-Host "  lib:     $libDir"
