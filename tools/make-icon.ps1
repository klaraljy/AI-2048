# Generates the AI-2048 desktop icon (.ico with a 256x256 frame).
#
# NOTE: This file is deliberately ASCII-only. Windows PowerShell 5.1 reads
# .ps1 files as ANSI unless they carry a UTF-8 BOM, so non-ASCII characters
# here turn into mojibake and break the parser. Keeping it ASCII means it
# runs under any PowerShell version and any code page, with no BOM required.
#
# ## Pattern
#
# ONE square tile filling almost the whole canvas, with the digits stacked in
# two rows because "2048" does not fit on one line:
#
#     +--------+
#     |  2 0   |
#     |  4 8   |
#     +--------+
#
# The tile uses the real in-game "32" tile look (#fe8b54 background, #fefcf7
# digits) and sits on the board colour so the rounded corners read against
# the desktop.
#
# ## Layout: how the two rows are placed
#
# The tile is split into two equal halves and each row is drawn centred in its
# own half using DrawString with Center/Center alignment. The halves tile the
# square exactly, so the two-line block is centred by construction -- no
# per-glyph bounds arithmetic, and left/right and top/bottom margins come out
# even automatically.
#
# ### Two abandoned approaches, recorded so nobody repeats them
#
# 1. Sizing the font from Graphics.MeasureString and centring on that box gave
#    a block clearly wider than tall. MeasureString returns the string's LINE
#    box, not the ink extent: at size 50 it reports 75.9 x 72.8 for "20" while
#    the glyphs are only 54.7 x 36.2. The reported height is about double the
#    real one.
# 2. Switching to GraphicsPath.GetBounds and solving the layout in closed form
#    was worse: the algebra said "exactly square", the drawn result measured
#    75 x 150. GraphicsPath.AddString's size argument does not mean the same
#    thing as Font.Size under GraphicsUnit.Pixel -- glyphs come out much
#    larger, by a size-dependent factor, so a ratio measured at one size does
#    not hold at another.
#
# Both were attempts to out-compute GDI+ text metrics. Letting DrawString do
# the centring sidesteps the whole problem.
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

$RowTop = '20'
$RowBottom = '48'

# --- Row layout: how tightly the two rows sit together -------------------
#
# The two rows share the middle band of the tile; RowBandInset is how much of
# the tile height is left empty at top and bottom. Smaller inset (0) would fill
# the whole tile; larger inset pulls the rows in and leaves more orange margin.
# 0.10 keeps a little margin while making the pair read as one tight block.
$RowBandInset = 0.10

# Font size as a fraction of the TILE size. With the rows pulled together the
# font can be a touch larger than before (0.42) and still fit both rows.
$FontRatio = 0.44

# --- Digit shadow --------------------------------------------------------
#
# A soft black drop shadow behind the white digits, offset down-right. Without
# it the white text sits flat on the orange tile and loses definition at small
# sizes (16px taskbar icon). Alpha is deliberately low: a hard black outline
# would look like a sticker rather than the game's flat tile style.
$ShadowAlpha = 70      # 0..255
$ShadowOffset = 1.0    # pixels at the 256px canvas; scales with the canvas

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

# Draws one icon at the given size. Everything scales off `size`.
function New-IconBitmap {
    param([int]$size)

    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
    $g.Clear([System.Drawing.ColorTranslator]::FromHtml($BoardBg))

    # One square tile filling almost the whole canvas.
    $margin = [Math]::Max(1, [int]($size * 0.035))
    $tile = $size - 2 * $margin
    $radius = [float]($tile * 0.18)

    $path = New-RoundedRectPath -X $margin -Y $margin -W $tile -H $tile -R $radius
    $brush = New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml($TileBg))
    $g.FillPath($brush, $path)
    $brush.Dispose()
    $path.Dispose()

    # Two rows, pulled together in the middle of the tile.
    #
    # Originally each row got its own half of the tile, which made "20" and "48"
    # sit far apart (the user's note: "上下断开的 20 和 48 大幅压缩行距，让视觉更紧凑").
    # Now both rows live inside the middle band and the two bands touch:
    #
    #     bandHeight = tile * (1 - 2 * RowBandInset) / 2
    #     top row    centred in [margin + inset, + bandHeight]
    #     bottom row centred in [margin + inset + bandHeight, ...]
    #
    # so the visual gap between the rows is just the font's own leading, and the
    # tile keeps a little breathing room at top and bottom. Rows touch, so the
    # same "halves tile exactly, centring is free" property still holds.
    $bandHeight = [float]($tile * (1.0 - 2.0 * $RowBandInset) / 2.0)
    $fontSize = [float]($tile * $FontRatio)
    $font = New-Object System.Drawing.Font('Segoe UI', $fontSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $fore = New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml($DigitColor))
    $shadow = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb($ShadowAlpha, 0, 0, 0))

    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = [System.Drawing.StringAlignment]::Center
    $fmt.LineAlignment = [System.Drawing.StringAlignment]::Center

    $bandTop = [float]($margin + $tile * $RowBandInset)
    $topRect = New-Object System.Drawing.RectangleF($margin, $bandTop, $tile, $bandHeight)
    $bottomRect = New-Object System.Drawing.RectangleF($margin, ($bandTop + $bandHeight), $tile, $bandHeight)

    # Shadow first, offset down-right, then the white digits on top. Without it
    # the white digits sit flat on the orange tile and read poorly at small sizes
    # (the user asked for "轻微的黑色投影或浮雕效果，使其与橙色背景分离").
    $shadowTop = New-Object System.Drawing.RectangleF(($margin + $ShadowOffset), ($bandTop + $ShadowOffset), $tile, $bandHeight)
    $shadowBottom = New-Object System.Drawing.RectangleF(($margin + $ShadowOffset), ($bandTop + $bandHeight + $ShadowOffset), $tile, $bandHeight)
    $g.DrawString($RowTop, $font, $shadow, $shadowTop, $fmt)
    $g.DrawString($RowBottom, $font, $shadow, $shadowBottom, $fmt)

    $g.DrawString($RowTop, $font, $fore, $topRect, $fmt)
    $g.DrawString($RowBottom, $font, $fore, $bottomRect, $fmt)

    # RectangleF is a struct — no Dispose() here (calling it fails the script).
    $fmt.Dispose(); $shadow.Dispose(); $fore.Dispose(); $font.Dispose()
    $g.Dispose()
    return $bmp
}

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
    $maskStride = [int]([math]::Floor(($size + 31) / 32) * 4)
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

    $zeros = New-Object byte[] $maskBytes
    $w.Write($zeros, 0, $maskBytes)

    $w.Flush()
    $bytes = $ms.ToArray()
    $w.Dispose(); $ms.Dispose()
    return @{ Bytes = $bytes; ImageSize = $imageSize }
}

# --- Render every size ---------------------------------------------------
# Each size is drawn natively rather than scaled down from 256: at 16px a
# 3.5% margin is under a pixel, and a downscaled tile loses the crisp edge.
$sizes = @(256, 128, 64, 48, 32, 16)
$frames = @()

foreach ($s in $sizes) {
    $bmp = New-IconBitmap -size $s
    $frame = New-DibFrame -Bitmap $bmp
    $bmp.Dispose()
    $frames += , @{ Size = $s; Bytes = $frame.Bytes; ImageSize = $frame.ImageSize }
    Write-Host ("  {0}x{0}  ({1} byte DIB)" -f $s, $frame.Bytes.Length)
}

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
