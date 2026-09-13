# Generates the AI-2048 Android launcher icons (mipmap PNGs) + adaptive icon XML.
#
# NOTE: This file is deliberately ASCII-only. Windows PowerShell 5.1 reads .ps1
# files as ANSI unless they carry a UTF-8 BOM, so non-ASCII characters here turn
# into mojibake and break the parser.
#
# ## Pattern (same as the desktop .ico, see tools/make-icon.ps1)
#
# ONE square tile filling almost the whole canvas, digits stacked in two rows
# because "2048" does not fit on one line:
#
#     +--------+
#     |  2 0   |
#     |  4 8   |
#     +--------+
#
# Tile uses the real in-game "32" tile look (#fe8b54 background, #fefcf7
# digits); it sits on the board colour (#9b7b78) so the rounded corners read.
#
# ## Adaptive icons (Android 8+)
#
# The system crops the icon to an OEM mask, so the safe zone matters:
#   - mipmap-anydpi-v26/ic_launcher.xml points at a foreground + background
#   - the foreground PNG is 108dp, of which only the middle 72dp is guaranteed
#     visible -> the tile is drawn at 66% of the canvas and centred
#   - the background is a flat colour resource (no second PNG needed)
#
# Legacy devices (< API 26) use the plain mipmap-*/ic_launcher.png files, which
# includes the board-colour margin baked in.
#
# Usage:  powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-android-icon.ps1

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$res = Join-Path $root 'android\app\src\main\res'

# In-game colours (must stay in sync with web/css/style.css :root)
$BoardBg = '#9b7b78'     # --board-bg
$TileBg = '#fe8b54'      # the "32" tile background
$DigitColor = '#fefcf7'  # --text-light

$RowTop = '20'
$RowBottom = '48'

# --- Row layout + shadow: MUST stay in sync with tools/make-icon.ps1 ------
#
# Same three knobs as the desktop .ico so both platforms look identical:
#   RowBandInset = how much tile height stays empty at top/bottom
#                  (rows are pulled together into the middle band)
#   FontRatio    = font size as a fraction of the tile
#   Shadow*      = soft black drop shadow behind the white digits
# See make-icon.ps1 for why: the user asked to compress the line spacing and
# to separate the digits from the orange background.
$RowBandInset = 0.10
$FontRatio = 0.44
$ShadowAlpha = 70
$ShadowOffset = 1.0

# Density buckets: name -> legacy launcher size in px (48dp at that density)
$Densities = [ordered]@{
  'mdpi'    = 48
  'hdpi'    = 72
  'xhdpi'   = 96
  'xxhdpi'  = 144
  'xxxhdpi' = 192
}

function New-RoundedPath([float]$x, [float]$y, [float]$w, [float]$h, [float]$radius) {
  $path = New-Object System.Drawing.Drawing2D.GraphicsPath
  $d = $radius * 2
  $path.AddArc($x, $y, $d, $d, 180, 90)
  $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
  $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
  $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
  $path.CloseFigure()
  return $path
}

# Draws the tile centred on a canvas of $Size px, occupying $Fill of it.
function New-IconBitmap([int]$Size, [double]$Fill, [bool]$WithBoard) {
  $bmp = New-Object System.Drawing.Bitmap($Size, $Size)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic

  $board = [System.Drawing.ColorTranslator]::FromHtml($BoardBg)
  $tile = [System.Drawing.ColorTranslator]::FromHtml($TileBg)
  $digit = [System.Drawing.ColorTranslator]::FromHtml($DigitColor)

  if ($WithBoard) {
    $g.Clear($board)
  } else {
    # Transparent background: needed for the adaptive foreground layer.
    $g.Clear([System.Drawing.Color]::Transparent)
  }

  $side = [float]($Size * $Fill)
  $origin = [float](($Size - $side) / 2)
  $radius = [float]($side * 0.16)

  $tilePath = New-RoundedPath $origin $origin $side $side $radius
  $tileBrush = New-Object System.Drawing.SolidBrush($tile)
  $g.FillPath($tileBrush, $tilePath)

  # Two rows, pulled together in the middle of the tile (same layout as the
  # desktop .ico — see make-icon.ps1 for the reasoning and the user's request).
  $bandHeight = [float]($side * (1.0 - 2.0 * $RowBandInset) / 2.0)
  $fontSize = [float]($side * $FontRatio)
  $font = New-Object System.Drawing.Font('Segoe UI', $fontSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
  $fore = New-Object System.Drawing.SolidBrush($digit)
  $shadow = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb($ShadowAlpha, 0, 0, 0))
  $fmt = New-Object System.Drawing.StringFormat
  $fmt.Alignment = [System.Drawing.StringAlignment]::Center
  $fmt.LineAlignment = [System.Drawing.StringAlignment]::Center

  $bandTop = [float]($origin + $side * $RowBandInset)
  # Shadow offset scales with the canvas so it reads the same at every density
  $shadowShift = [float]($side * $ShadowOffset / 256.0)

  $topRect = New-Object System.Drawing.RectangleF($origin, $bandTop, $side, $bandHeight)
  $bottomRect = New-Object System.Drawing.RectangleF($origin, ($bandTop + $bandHeight), $side, $bandHeight)
  $shadowTop = New-Object System.Drawing.RectangleF(($origin + $shadowShift), ($bandTop + $shadowShift), $side, $bandHeight)
  $shadowBottom = New-Object System.Drawing.RectangleF(($origin + $shadowShift), ($bandTop + $bandHeight + $shadowShift), $side, $bandHeight)

  $g.DrawString($RowTop, $font, $shadow, $shadowTop, $fmt)
  $g.DrawString($RowBottom, $font, $shadow, $shadowBottom, $fmt)
  $g.DrawString($RowTop, $font, $fore, $topRect, $fmt)
  $g.DrawString($RowBottom, $font, $fore, $bottomRect, $fmt)

  # RectangleF is a struct — no Dispose() (calling it throws "MethodNotFound").
  $fmt.Dispose(); $shadow.Dispose(); $fore.Dispose(); $font.Dispose()
  $tileBrush.Dispose(); $tilePath.Dispose(); $g.Dispose()
  return $bmp
}

# ---- 1. legacy launcher icons (board-colour margin baked in) ----
foreach ($entry in $Densities.GetEnumerator()) {
  $dir = Join-Path $res ("mipmap-" + $entry.Key)
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $size = $entry.Value
  $bmp = New-IconBitmap -Size $size -Fill 0.86 -WithBoard $true
  $out = Join-Path $dir 'ic_launcher.png'
  $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  Write-Host ("  wrote {0} ({1}x{1})" -f $out.Replace($root + '\', ''), $size)
}

# ---- 2. adaptive foreground: 108dp canvas, tile in the safe 66% ----
foreach ($entry in $Densities.GetEnumerator()) {
  $dir = Join-Path $res ("mipmap-" + $entry.Key)
  $size = [int]($entry.Value * 108 / 48)   # 48dp canvas -> 108dp canvas
  $bmp = New-IconBitmap -Size $size -Fill 0.66 -WithBoard $false
  $out = Join-Path $dir 'ic_launcher_foreground.png'
  $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  Write-Host ("  wrote {0} ({1}x{1})" -f $out.Replace($root + '\', ''), $size)
}

# ---- 3. adaptive icon XML + background colour ----
$anydpi = Join-Path $res 'mipmap-anydpi-v26'
New-Item -ItemType Directory -Force -Path $anydpi | Out-Null
$adaptive = @'
<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@color/ic_launcher_background" />
    <foreground android:drawable="@mipmap/ic_launcher_foreground" />
</adaptive-icon>
'@
[System.IO.File]::WriteAllText((Join-Path $anydpi 'ic_launcher.xml'), $adaptive, (New-Object System.Text.UTF8Encoding($false)))

$valuesDir = Join-Path $res 'values'
$colorsPath = Join-Path $valuesDir 'colors.xml'
$colors = @"
<?xml version="1.0" encoding="utf-8"?>
<resources>
    <!-- Board background colour, so the adaptive icon's outer ring matches the game. -->
    <color name="ic_launcher_background">$BoardBg</color>
</resources>
"@
[System.IO.File]::WriteAllText($colorsPath, $colors, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "  wrote mipmap-anydpi-v26/ic_launcher.xml + values/colors.xml"

Write-Host "Done. Now add android:icon=`"@mipmap/ic_launcher`" to AndroidManifest.xml if missing." -ForegroundColor Yellow
