# Makes the painted-out sides of a widened picture match the original next to them, in its 1x, 2x
# and 4x versions: wallpaper gets the original's fine pattern and colour (mirror-tiled from a plain
# patch of it), and gold gets the original's crispness, lightness and colour.
#
#   .\match_margins.ps1 -Id 4502 -Wall 649,165,685,440
#
# -Wall is a patch of plain wallpaper in the 1x widened picture. The pictures are changed in place;
# originals are kept as <name>.before-match.png the first time.
param(
    [Parameter(Mandatory)][int]$Id,
    [string]$Wall,                                # x0,y0,x1,y1
    [int]$Margin = 128,
    [double]$GoldGain = 2.0,
    [double]$GoldLift = 0.06,
    [double]$GoldChroma = 1.15
)
$ErrorActionPreference = "Stop"
$WallRect = if ($Wall) { [int[]]($Wall -split ',') } else { $null }
Add-Type -AssemblyName System.Drawing
Add-Type -Path (Join-Path $PSScriptRoot "ImageOps.cs") -ReferencedAssemblies System.Drawing
$assets = Join-Path $PSScriptRoot "..\assets"
$files = @(@((Join-Path $assets "wide\$Id.png"), 1), @((Join-Path $assets "x2\_wide\$Id.png"), 2), @((Join-Path $assets "x4\_wide\$Id.png"), 4))

foreach ($f in $files) {
    $path = $f[0]; $k = $f[1]
    if (-not (Test-Path $path)) { continue }
    $backup = $path -replace '\.png$', '.before-match.png'
    if (-not (Test-Path $backup)) { Copy-Item $path $backup }
    $b = [Drawing.Bitmap]::FromFile($backup); $w = $b.Width; $h = $b.Height
    $px = [ImageOps]::Read($b); $b.Dispose()
    $m = $Margin * $k
    # Each side is corrected out to 24 columns into the original, fading in from there to full
    # strength 12 columns into the painted side, so there is no edge at the join.
    $in = 24 * $k; $full = 12 * $k
    foreach ($side in @(@(0, ($m + $in), ($m + $in), ($m - $full)), @(($w - $m - $in), $w, ($w - $m - $in), ($w - $m + $full)))) {
        if ($WallRect) {
            [ImageOps]::ReplaceWallpaper($px, $w, $h, $side[0], $side[1], $WallRect[0] * $k, $WallRect[1] * $k, $WallRect[2] * $k, $WallRect[3] * $k, 6 * $k, $side[2], $side[3])
        }
        [ImageOps]::SharpenGold($px, $w, $h, $side[0], $side[1], 2 * $k, $GoldGain, $GoldLift, $GoldChroma, $side[2], $side[3])
    }
    $out = [ImageOps]::Write($px, $w, $h)
    $out.Save($path, [Drawing.Imaging.ImageFormat]::Png); $out.Dispose()
    "matched $path"
}
