# Generates the AI-2048 desktop icon (.ico with a 256x256 frame).
#
# NOTE: This file is deliberately ASCII-only. Windows PowerShell 5.1 reads
# .ps1 files as ANSI unless they carry a UTF-8 BOM, so non-ASCII characters
# here turn into mojibake and break the parser. Keeping it ASCII means it
# runs under any PowerShell version and any code page, with no BOM required.
#
# ## Pattern
#
# ONE tile, filling almost the whole canvas, with the digits stacked in two
# rows because "2048" does not fit on one line:
#
#     +--------+
#     |  2 0   |
#     |  4 8   |
#     +--------+
#
# The tile uses the colours and corner radius of the real in-game "32" tile
# (#fe8b54 background, #fefcf7 digits), and sits on the board colour so the
# rounded corners read against the desktop.
#
# ## Usage
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-icon.ps1
#
# Output: assets\icon\ai2048.ico  (256/128/64/48/32/16)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$RepoRoot = Split-Path -Parent $PSScriptRoot
$OutDir = Join-Path $RepoRoot 'assets\icon'
$OutFile = Join-Path $OutDir 'ai2048.ico'
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

# --- Palette taken from web\css\style.css --------------------------------
# If the front-end palette changes, these must change with it.
$BoardBg = '#9b7b78'    # --board-bg
$TileBg = '#fe8b54'     # the "32" tile background
$DigitColor = '#fefcf7' # --text-light, the colour the game uses on that tile

# Two rows of two digits. Kept as separate strings so the row gap stays
# independent of the glyph metrics.
$RowTop = '20'
$RowBottom = '48'

function New-RoundedRectPath {
    param([float]$X, [float]$Y, [float]$W, [float]$H, [float]$R)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $R * 2
    $path.AddArc($X, $Y, $d, $d, 180, 90)
    $path.AddArc($X + $W - $d, $Y, $d, $d, 270, 90)
    $path.AddArc($X + $W - $d, $Y + $H - $d, $d, $d, 0, 90)
    $path.AddArc($X, $Y + $H - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

# Draws one row of digits centred on (cx, cy).
function Draw-CenteredText {
    param(
        [System.Drawing.Graphics]$G,
        [string]$Text,
        [System.Drawing.Font]$Font,
        [System.Drawing.Brush]$Brush,
        [float]$Cx,
        [float]$Cy
    )
    $size = $G.MeasureString($Text, $Font)
    $g.DrawString($Text, $Font, $Brush, $Cx - $size.Width / 2, $Cy - $size.Height / 2)
}

# Draws the whole icon at baseSize and returns a new bitmap.
function New-IconBitmap {
    param([int]$baseSize)

    $bmp = New-Object System.Drawing.Bitmap($baseSize, $baseSize, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
    $g.Clear([System.Drawing.ColorTranslator]::FromHtml($BoardBg))

    # One tile filling almost the whole canvas: thin margin all round.
    $margin = [int]($baseSize * 0.055)
    $tile = $baseSize - 2 * $margin
    $radius = [float]($tile * 0.18)

    $path = New-RoundedRectPath -X $margin -Y $margin -W $tile -H $tile -R $radius
    $brush = New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml($TileBg))
    $g.FillPath($brush, $path)
    $brush.Dispose()
    $path.Dispose()

    # Font sized off the tile WIDTH, not the height: two rows have to fit in
    # the height, so a width-based size would overflow. 0.46 keeps both rows
    # inside the tile with a comfortable gap and still fills the width.
    $fontSize = [float]($tile * 0.46)
    $font = New-Object System.Drawing.Font('Segoe UI', $fontSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $fore = New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml($DigitColor))

    $cx = $baseSize / 2.0
    # Rows sit at 1/4 and 3/4 of the tile: optically balanced, equal gap.
    $rowGap = $tile * 0.235
    $cy = $margin + $tile / 2.0
    Draw-CenteredText -G $g -Text $RowTop -Font $font -Brush $fore -Cx $cx -Cy ($cy - $rowGap)
    Draw-CenteredText -G $g -Text $RowBottom -Font $font -Brush $fore -Cx $cx -Cy ($cy + $rowGap)

    $fore.Dispose(); $font.Dispose()
    $g.Dispose()
    return $bmp
}

# --- Render every size ---------------------------------------------------
$sizes = @(256, 128, 64, 48, 32, 16)
$frames = @()

$master = New-IconBitmap -baseSize 256

# A frame as a DIB (BITMAPINFOHEADER + bottom-up BGRA pixels + AND mask).
#
# Why DIB and not PNG: .NET's System.Drawing.Icon cannot decode PNG frames
# inside an .ico -- it throws "Requested range extends past the end of the
# array". Explorer itself handles PNG frames (Vista+), but anything going
# through the older Win32/GDI+ icon path does not. Since this file is meant to
# be a desktop shortcut icon, maximum compatibility beats file size.
#
# The AND mask is all zeros ("every pixel is opaque"). The 32bpp alpha channel
# carries the real transparency; the mask only matters for very old shells,
# but the header requires it to be present and correctly sized.
function New-DibFrame {
    param([System.Drawing.Bitmap]$Bitmap)

    $size = $Bitmap.Width
    $stride = $size * 4
    $maskStride = [int]([math]::Floor(($size + 31) / 32) * 4)   # 1bpp rows, padded to 4 bytes
    $maskBytes = $maskStride * $size
    $pixelBytes = $stride * $size

    # Height in the header is doubled: XOR (colour) image + AND (mask) image.
    $headerSize = 40
    $imageSize = $headerSize + $pixelBytes + $maskBytes

    $ms = New-Object System.IO.MemoryStream
    $w = New-Object System.IO.BinaryWriter($ms)

    $w.Write([UInt32]$headerSize)
    $w.Write([Int32]$size)          # width
    $w.Write([Int32]($size * 2))    # height (doubled)
    $w.Write([UInt16]1)             # planes
    $w.Write([UInt16]32)            # bits per pixel
    $w.Write([UInt32]0)             # BI_RGB, no compression
    $w.Write([UInt32]$pixelBytes)
    $w.Write([Int32]0)              # x pixels per meter
    $w.Write([Int32]0)              # y pixels per meter
    $w.Write([UInt32]0)             # colours used
    $w.Write([UInt32]0)             # important colours

    # Pixels, bottom-up, BGRA order.
    for ($y = $size - 1; $y -ge 0; $y--) {
        for ($x = 0; $x -lt $size; $x++) {
            $c = $Bitmap.GetPixel($x, $y)
            $w.Write([Byte]$c.B)
            $w.Write([Byte]$c.G)
            $w.Write([Byte]$c.R)
            $w.Write([Byte]$c.A)
        }
    }

    # AND mask: all zero = fully opaque.
    $zeros = New-Object byte[] $maskBytes
    $w.Write($zeros, 0, $maskBytes)

    $w.Flush()
    $bytes = $ms.ToArray()
    $w.Dispose(); $ms.Dispose()
    return @{ Bytes = $bytes; ImageSize = $imageSize }
}

foreach ($s in $sizes) {
    # Small sizes get a heavier margin: at 16px a 5.5% margin is under one
    # pixel and the tile looks like it bleeds off the edge.
    if ($s -le 32) {
        $small = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $sg = [System.Drawing.Graphics]::FromImage($small)
        $sg.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $sg.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
        $sg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $sg.Clear([System.Drawing.ColorTranslator]::FromHtml($BoardBg))
        $m = [Math]::Max(1, [int]($s * 0.06))
        $t = $s - 2 * $m
        $p = New-RoundedRectPath -X $m -Y $m -W $t -H $t -R ([float]($t * 0.18))
        $b = New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml($TileBg))
        $sg.FillPath($b, $p)
        $b.Dispose(); $p.Dispose()
        $f = New-Object System.Drawing.Font('Segoe UI', [float]($t * 0.46), [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
        $fb = New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml($DigitColor))
        $cx = $s / 2.0
        $cy = $m + $t / 2.0
        $gap = $t * 0.235
        Draw-CenteredText -G $sg -Text $RowTop -Font $f -Brush $fb -Cx $cx -Cy ($cy - $gap)
        Draw-CenteredText -G $sg -Text $RowBottom -Font $f -Brush $fb -Cx $cx -Cy ($cy + $gap)
        $fb.Dispose(); $f.Dispose(); $sg.Dispose()
        $frame = New-DibFrame -Bitmap $small
        $small.Dispose()
    } else {
        $scaled = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $sg = [System.Drawing.Graphics]::FromImage($scaled)
        $sg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $sg.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $sg.Clear([System.Drawing.ColorTranslator]::FromHtml($BoardBg))
        $sg.DrawImage($master, 0, 0, $s, $s)
        $sg.Dispose()
        $frame = New-DibFrame -Bitmap $scaled
        $scaled.Dispose()
    }

    $frames += , @{ Size = $s; Bytes = $frame.Bytes; ImageSize = $frame.ImageSize }
    Write-Host ("  {0}x{0}  ({1} byte DIB)" -f $s, $frame.Bytes.Length)
}
$master.Dispose()

# --- Assemble the ICO ----------------------------------------------------
# Layout: 6-byte header + 16-byte directory entry per frame + frame payloads.
$fs = [System.IO.File]::Create($OutFile)
$bw = New-Object System.IO.BinaryWriter($fs)

$bw.Write([UInt16]0)                 # reserved
$bw.Write([UInt16]1)                 # type 1 = icon
$bw.Write([UInt16]$frames.Count)     # image count

# Width/height in a directory entry are single bytes; 256 is encoded as 0.
$offset = 6 + 16 * $frames.Count
foreach ($f in $frames) {
    $dim = if ($f.Size -ge 256) { 0 } else { $f.Size }
    $bw.Write([Byte]$dim)            # width
    $bw.Write([Byte]$dim)            # height
    $bw.Write([Byte]0)               # palette colour count (0 = no palette)
    $bw.Write([Byte]0)               # reserved
    $bw.Write([UInt16]1)             # colour planes
    $bw.Write([UInt16]32)            # bits per pixel
    $bw.Write([UInt32]$f.Bytes.Length)
    $bw.Write([UInt32]$offset)
    $offset += $f.Bytes.Length
}
foreach ($f in $frames) { $bw.Write($f.Bytes) }

$bw.Flush(); $bw.Dispose(); $fs.Dispose()

$info = Get-Item $OutFile
Write-Host ''
Write-Host ("Wrote {0}" -f $OutFile)
Write-Host ("  {0} bytes, {1} frames (largest 256x256)" -f $info.Length, $frames.Count)
