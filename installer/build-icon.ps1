# build-icon.ps1
#
# Renders the matata application icon at multiple sizes via System.Drawing
# and packs them into installer\matata.ico (multi-resolution PNG-encoded
# entries -- the format Windows 10/11 prefers for crisp scaling).
#
# Usage: powershell -ExecutionPolicy Bypass -File installer\build-icon.ps1

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$outIco  = Join-Path $PSScriptRoot "matata.ico"

$sizes = 16, 24, 32, 48, 64, 128, 256

function New-Color { param([int]$a, [int]$r, [int]$g, [int]$b)
    [System.Drawing.Color]::FromArgb($a, $r, $g, $b)
}
function New-Pt { param([single]$x, [single]$y)
    New-Object System.Drawing.PointF -ArgumentList $x, $y
}

function New-MatataPng {
    param([int]$size)
    $bmp = New-Object System.Drawing.Bitmap -ArgumentList $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode      = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.InterpolationMode  = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode    = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    # Rounded-square background path (Win11-ish corner radius).
    $cornerR = [Math]::Max(2, [int]($size * 0.22))
    $d       = $cornerR * 2
    $rectX   = 0
    $rectY   = 0
    $rectR   = $size - 1
    $rectB   = $size - 1
    $path    = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc($rectX,           $rectY,           $d, $d, 180, 90) | Out-Null
    $path.AddArc(($rectR - $d),    $rectY,           $d, $d, 270, 90) | Out-Null
    $path.AddArc(($rectR - $d),    ($rectB - $d),    $d, $d,   0, 90) | Out-Null
    $path.AddArc($rectX,           ($rectB - $d),    $d, $d,  90, 90) | Out-Null
    $path.CloseFigure()

    # Background gradient: vibrant matata green top -> deeper green bottom.
    $gradTop = New-Color 255 70  198 88
    $gradBot = New-Color 255 22  120 50
    $p1 = New-Pt 0 0
    $p2 = New-Pt 0 $size
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush -ArgumentList $p1, $p2, $gradTop, $gradBot
    $g.FillPath($brush, $path)
    $brush.Dispose()

    # Subtle gloss along the top half.
    $hp1 = New-Pt 0 0
    $hp2 = New-Pt 0 ([single]($size * 0.55))
    $hC1 = New-Color 70 255 255 255
    $hC2 = New-Color 0  255 255 255
    $highlight = New-Object System.Drawing.Drawing2D.LinearGradientBrush -ArgumentList $hp1, $hp2, $hC1, $hC2
    $g.FillPath($highlight, $path)
    $highlight.Dispose()

    # Hairline edge.
    $edgeColor = New-Color 60 0 0 0
    $edgeWidth = [single][Math]::Max(1.0, $size / 128.0)
    $edge = New-Object System.Drawing.Pen -ArgumentList $edgeColor, $edgeWidth
    $g.DrawPath($edge, $path)
    $edge.Dispose()

    # Arrow geometry (in pixel space).
    $cx          = [single]($size * 0.5)
    $arrowTop    = [single]($size * 0.18)
    $arrowMidY   = [single]($size * 0.58)
    $shaftHalf   = [single]($size * 0.085)
    $headHalf    = [single]($size * 0.22)
    $headTop     = [single]($size * 0.40)
    $trayTop     = [single]($size * 0.70)
    $trayHeight  = [single]($size * 0.085)
    $trayInset   = [single]($size * 0.20)

    $pts = @(
        (New-Pt ($cx - $shaftHalf) $arrowTop),
        (New-Pt ($cx + $shaftHalf) $arrowTop),
        (New-Pt ($cx + $shaftHalf) $headTop),
        (New-Pt ($cx + $headHalf)  $headTop),
        (New-Pt $cx                $arrowMidY),
        (New-Pt ($cx - $headHalf)  $headTop),
        (New-Pt ($cx - $shaftHalf) $headTop)
    )

    $offset = [single][Math]::Max(1.0, $size / 96.0)
    $shadowPts = @()
    foreach ($p in $pts) { $shadowPts += (New-Pt $p.X ($p.Y + $offset)) }

    $shadowBrush = New-Object System.Drawing.SolidBrush -ArgumentList (New-Color 60 0 0 0)
    $shadow = New-Object System.Drawing.Drawing2D.GraphicsPath
    $shadow.AddPolygon($shadowPts) | Out-Null
    $g.FillPath($shadowBrush, $shadow)
    $shadow.Dispose()

    $whiteBrush = New-Object System.Drawing.SolidBrush -ArgumentList (New-Color 255 255 255 255)
    $arrow = New-Object System.Drawing.Drawing2D.GraphicsPath
    $arrow.AddPolygon($pts) | Out-Null
    $g.FillPath($whiteBrush, $arrow)
    $arrow.Dispose()

    # Tray bar (rounded ends).
    $trayLeft  = $trayInset
    $trayWidth = [single]($size - 2 * $trayInset)
    $r         = [single]($trayHeight * 0.5)
    $trayPath  = New-Object System.Drawing.Drawing2D.GraphicsPath
    $trayPath.AddArc($trayLeft, $trayTop, ($r * 2), $trayHeight, 90, 180) | Out-Null
    $trayPath.AddArc(($trayLeft + $trayWidth - $r * 2), $trayTop, ($r * 2), $trayHeight, 270, 180) | Out-Null
    $trayPath.CloseFigure()

    # Tray shadow.
    $trayShadow = New-Object System.Drawing.Drawing2D.GraphicsPath
    $trayShadow.AddArc($trayLeft, ($trayTop + $offset), ($r * 2), $trayHeight, 90, 180) | Out-Null
    $trayShadow.AddArc(($trayLeft + $trayWidth - $r * 2), ($trayTop + $offset), ($r * 2), $trayHeight, 270, 180) | Out-Null
    $trayShadow.CloseFigure()
    $g.FillPath($shadowBrush, $trayShadow)
    $trayShadow.Dispose()

    $g.FillPath($whiteBrush, $trayPath)
    $trayPath.Dispose()

    $shadowBrush.Dispose()
    $whiteBrush.Dispose()
    $g.Dispose()
    $path.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $ms.ToArray()
}

# Render each PNG into memory.
$images = @()
foreach ($s in $sizes) {
    $bytes = New-MatataPng -size $s
    $images += [PSCustomObject]@{ Size = $s; Bytes = $bytes }
    Write-Host ("rendered {0}x{0} -> {1} bytes" -f $s, $bytes.Length)
}

# Pack into ICO.
$bw = New-Object System.IO.MemoryStream
$w  = New-Object System.IO.BinaryWriter -ArgumentList $bw
$w.Write([uint16]0)               # reserved
$w.Write([uint16]1)               # type 1 = icon
$w.Write([uint16]$images.Count)

$cursor = 6 + ($images.Count * 16)

foreach ($img in $images) {
    $sz = if ($img.Size -ge 256) { 0 } else { [byte]$img.Size }
    $w.Write([byte]$sz)
    $w.Write([byte]$sz)
    $w.Write([byte]0)               # palette
    $w.Write([byte]0)               # reserved
    $w.Write([uint16]1)             # planes
    $w.Write([uint16]32)            # bits/pixel
    $w.Write([uint32]$img.Bytes.Length)
    $w.Write([uint32]$cursor)
    $cursor += $img.Bytes.Length
}
foreach ($img in $images) {
    $b = [byte[]]$img.Bytes
    $w.Write($b, 0, $b.Length)
}
$w.Flush()
[System.IO.File]::WriteAllBytes($outIco, $bw.ToArray())
$bw.Dispose()

Write-Host ""
Write-Host "[ok] $outIco"
Write-Host ("     {0} bytes ({1} sizes packed)" -f (Get-Item $outIco).Length, $images.Count)
