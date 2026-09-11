# Generates the AI-2048 desktop icon (.ico with a 256x256 frame).
#
# NOTE: This file is deliberately ASCII-only. Windows PowerShell 5.1 reads
# .ps1 files as ANSI unless they carry a UTF-8 BOM, so non-ASCII characters
# here turn into mojibake and break the parser. Keeping it ASCII means it
# runs under any PowerShell version and any code page, with no BOM required.
#
# ## Pattern
#
# Four tiles in a 2x2 grid, filling almost the whole canvas:
#
#     2  0
#     4  8
#
# Each tile uses the colours and corner radius of a real in-game tile.
#
# ## One adjustment that is not optional
#
# In the game, the "2" tile is #fef0de (near white) and its digit is drawn
# WHITE -- that works because it sits on a dark board. On its own there is no
# dark board behind it, so copying that would give white text on a near-white
# tile: invisible, especially at 16x16.
#
# So this icon:
#   - fills the canvas with the board colour (--board-bg #9b7b78) as backing
#   - draws every digit in the game's dark text colour (#434b54)
#
# The palette is still the game's own; only the digit colour is forced dark.
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

# All four tiles share the "32" tile's look, per the icon spec.
$Digits = @('2', '0', '4', '8')

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

# Draws the whole icon at baseSize and returns a new bitmap.
function New-IconBitmap {
    param([int]$baseSize)

    $bmp = New-Object System.Drawing.Bitmap($baseSize, $baseSize, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
    $g.Clear([System.Drawing.ColorTranslator]::FromHtml($BoardBg))

    # Tiles fill almost the whole canvas: thin outer margin, thin seam between
    # tiles (the seam shows the board colour, same as in the game).
    $margin = [int]($baseSize * 0.055)
    $gap = [int]($baseSize * 0.055)
    $tile = [int](($baseSize - 2 * $margin - $gap) / 2)
    $radius = [float]($tile * 0.20)

    for ($i = 0; $i -lt 4; $i++) {
        $col = $i % 2
        $row = [int][math]::Floor($i / 2)
        $x = $margin + $col * ($tile + $gap)
        $y = $margin + $row * ($tile + $gap)

        $path = New-RoundedRectPath -X $x -Y $y -W $tile -H $tile -R $radius
        $brush = New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml($TileBg))
        $g.FillPath($brush, $path)
        $brush.Dispose()
        $path.Dispose()

        # Font size ~68% of the tile width: digit fills the tile without spilling.
        $fontSize = [float]($tile * 0.68)
        $font = New-Object System.Drawing.Font('Segoe UI', $fontSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
        $fore = New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml($DigitColor))

        $fmt = New-Object System.Drawing.StringFormat
        $fmt.Alignment = [System.Drawing.StringAlignment]::Center
        $fmt.LineAlignment = [System.Drawing.StringAlignment]::Center
        $rect = New-Object System.Drawing.RectangleF($x, $y, $tile, $tile)
        $g.DrawString($Digits[$i], $font, $fore, $rect, $fmt)

        $fmt.Dispose(); $fore.Dispose(); $font.Dispose()
    }

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
    $scaled = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $sg = [System.Drawing.Graphics]::FromImage($scaled)
    $sg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $sg.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $sg.Clear([System.Drawing.ColorTranslator]::FromHtml($BoardBg))
    $sg.DrawImage($master, 0, 0, $s, $s)
    $sg.Dispose()

    $frame = New-DibFrame -Bitmap $scaled
    $frames += , @{ Size = $s; Bytes = $frame.Bytes; ImageSize = $frame.ImageSize }
    $scaled.Dispose()

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
# For DIB frames the "bytes in resource" field counts the payload only, while
# "image size" is the BITMAPINFOHEADER size field (header + pixels + mask).
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
