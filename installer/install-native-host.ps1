# install-native-host.ps1
#
# Run by Inno Setup as a post-install step. Writes the native-messaging
# host JSON files into {app}\native-host\ with the "path" field pointing
# at the just-installed matata-host.exe, and registers HKCU keys for
# Chrome / Edge / Chromium / Brave / Firefox.
#
# Preserves any existing allowed_origins (chromium) / allowed_extensions
# (Firefox) found in the previously-registered manifests so users who
# already paired their browsers via install-extension.bat don't have to
# redo it after every matata upgrade.
#
# Usage: powershell -ExecutionPolicy Bypass -File install-native-host.ps1 -AppDir "C:\Path\to\matata"

param(
    [Parameter(Mandatory = $true)]
    [string]$AppDir
)

$ErrorActionPreference = "Stop"

$browsers = @(
    @{ Name = 'Chrome';   Key = 'HKCU:\Software\Google\Chrome\NativeMessagingHosts\com.matata.host';                  Firefox = $false },
    @{ Name = 'Edge';     Key = 'HKCU:\Software\Microsoft\Edge\NativeMessagingHosts\com.matata.host';                 Firefox = $false },
    @{ Name = 'Chromium'; Key = 'HKCU:\Software\Chromium\NativeMessagingHosts\com.matata.host';                       Firefox = $false },
    @{ Name = 'Brave';    Key = 'HKCU:\Software\BraveSoftware\Brave-Browser\NativeMessagingHosts\com.matata.host';    Firefox = $false },
    @{ Name = 'Firefox';  Key = 'HKCU:\Software\Mozilla\NativeMessagingHosts\com.matata.host';                        Firefox = $true  }
)

$hostExe = Join-Path $AppDir 'matata-host.exe'
$hostDir = Join-Path $AppDir 'native-host'
if (-not (Test-Path $hostDir)) {
    New-Item -Path $hostDir -ItemType Directory | Out-Null
}
$hostJsonChrome  = Join-Path $hostDir 'com.matata.host.json'
$hostJsonFirefox = Join-Path $hostDir 'com.matata.host.firefox.json'
# Firefox gecko IDs we always allow:
#   matata-self@matata.local -- self-distributed XPI (signed via AMO unlisted)
#   matata@matata.local      -- listed AMO submission (legacy / future)
# Both IDs share one matata-host.exe; whichever entry the user installs from
# will work without re-registering anything.
$ffDefaultIds = @('matata-self@matata.local', 'matata@matata.local')

function Get-RegisteredJsonPath {
    param([string]$Key)
    try {
        $val = (Get-ItemProperty -Path $Key -ErrorAction Stop).'(default)'
        if ($val -and (Test-Path -LiteralPath $val)) { return $val }
    } catch {}
    return $null
}

# Preserve chromium-family allowed_origins.
$chromeAllowed = @()
foreach ($b in $browsers) {
    if ($b.Firefox) { continue }
    $p = Get-RegisteredJsonPath -Key $b.Key
    if ($null -eq $p) { continue }
    try {
        $j = Get-Content -LiteralPath $p -Raw | ConvertFrom-Json
        if ($j.allowed_origins) { $chromeAllowed += $j.allowed_origins }
    } catch {}
}
$chromeAllowed = @($chromeAllowed | Sort-Object -Unique)

# Preserve Firefox allowed_extensions; ensure the gecko id is always present.
$ffAllowed = @($ffDefaultIds)
$ffPrev = Get-RegisteredJsonPath -Key 'HKCU:\Software\Mozilla\NativeMessagingHosts\com.matata.host'
if ($ffPrev) {
    try {
        $j = Get-Content -LiteralPath $ffPrev -Raw | ConvertFrom-Json
        if ($j.allowed_extensions) { $ffAllowed += $j.allowed_extensions }
    } catch {}
}
$ffAllowed = @($ffAllowed | Sort-Object -Unique)

# Build manifests. Use [ordered] hashtable so ConvertTo-Json keeps a
# stable field order.
$chromeManifest = [ordered]@{
    name            = 'com.matata.host'
    description     = 'matata download helper native host'
    path            = $hostExe
    type            = 'stdio'
    allowed_origins = $chromeAllowed
}
$ffManifest = [ordered]@{
    name               = 'com.matata.host'
    description        = 'matata download helper native host'
    path               = $hostExe
    type               = 'stdio'
    allowed_extensions = $ffAllowed
}

# Write JSONs as UTF-8 without BOM. Chrome's native-messaging host loader
# is BOM-tolerant, but a clean UTF-8 file is the lowest-friction choice.
$utf8NoBom = New-Object System.Text.UTF8Encoding $false
[System.IO.File]::WriteAllText($hostJsonChrome,  ($chromeManifest | ConvertTo-Json -Depth 4), $utf8NoBom)
[System.IO.File]::WriteAllText($hostJsonFirefox, ($ffManifest     | ConvertTo-Json -Depth 4), $utf8NoBom)

# Register HKCU keys (default value = path to JSON manifest).
foreach ($b in $browsers) {
    $target = if ($b.Firefox) { $hostJsonFirefox } else { $hostJsonChrome }
    if (-not (Test-Path $b.Key)) {
        New-Item -Path $b.Key -Force | Out-Null
    }
    Set-ItemProperty -Path $b.Key -Name '(default)' -Value $target -Type String
}

Write-Host "[matata] native-messaging host registered:"
Write-Host "  host exe:        $hostExe"
Write-Host "  chromium json:   $hostJsonChrome"
Write-Host "  firefox json:    $hostJsonFirefox"
Write-Host ("  chromium IDs:    {0}" -f ($chromeAllowed -join ', '))
Write-Host ("  firefox IDs:     {0}" -f ($ffAllowed     -join ', '))
