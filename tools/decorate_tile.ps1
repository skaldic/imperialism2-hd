# Paints a decorative variant of a terrain tile with the image model: the tile is laid out 3x3
# (so the model sees it repeat) and only the middle of the centre copy is repainted, leaving a
# border of the original so the variant still joins its neighbours seamlessly. Writes the 2x tile
# to assets\decor\<name>.png.
#
#   .\decorate_tile.ps1 -Tile 10000 -Name cottage1 -Prompt "..." -Seed 1
param(
    [Parameter(Mandatory)][string]$Tile,          # id in assets\x2\MapTiles
    [Parameter(Mandatory)][string]$Name,
    [Parameter(Mandatory)][string]$Prompt,
    [string]$Negative = "floating island, diorama, platform, base, raised ground, cliff edge, circle, top-down, perspective, 3d render, ring, shadow ring, border, frame, text, letters, watermark, people, river, water, castle, city, photo, blurry",
    [int]$Seed = 1,
    [switch]$Single,
    [switch]$Reuse,                               # keep an earlier painting (_work\<name>-raw.png), only lay it again
    [int]$Open = 1,                               # drop subject parts thinner than 2*Open+1 pixels
    [int]$Tolerance = 90,                         # colour distance from the grass that counts as subject
    [int]$Border = 14,                            # 2x pixels of the tile kept at each edge
    [int]$Steps = 32,
    [double]$Cfg = 7,
    [string]$Sd = "",
    [string]$Model = "",
    [string]$Vae = ""
)
# External tools default to the tools folder (tools\tools_dir.ps1); set here, as $PSScriptRoot is
# not available in parameter defaults of a script with mandatory parameters.
if (-not $Sd) { $Sd = Join-Path (& "$PSScriptRoot\tools_dir.ps1") "sdcpp\bin\sd-cli.exe" }
if (-not $Model) { $Model = Join-Path (& "$PSScriptRoot\tools_dir.ps1") "sdcpp\models\sdxl_inpaint.safetensors" }
if (-not $Vae) { $Vae = Join-Path (& "$PSScriptRoot\tools_dir.ps1") "sdcpp\models\sdxl_vae_fp16_fix.safetensors" }
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$assets = Join-Path $PSScriptRoot "..\assets"
$out = Join-Path $assets "decor"; $work = Join-Path $out "_work"
New-Item -ItemType Directory -Force $out, $work | Out-Null
$src = [Drawing.Bitmap]::FromFile((Resolve-Path (Join-Path $assets "x2\MapTiles\$Tile.png")))
$T = $src.Width                                    # 128
$S = 1024; $k = $S / (3 * $T)                      # the 3x3 block at the model's size
if ($Single) { $k = $S / $T }                     # or the tile alone, for subjects that fill it
$canvas = New-Object Drawing.Bitmap $S, $S, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
$g = [Drawing.Graphics]::FromImage($canvas); $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
$n3 = if ($Single) { 1 } else { 3 }; $off = if ($Single) { 0 } else { $T }
for ($ty = 0; $ty -lt $n3; $ty++) { for ($tx = 0; $tx -lt $n3; $tx++) {
    $g.DrawImage($src, [int]($tx * $T * $k), [int]($ty * $T * $k), [int]($T * $k) + 1, [int]($T * $k) + 1) } }
$g.Dispose(); $src.Dispose()
$initPath = Join-Path $work "$Name-init.png"; $canvas.Save($initPath, [Drawing.Imaging.ImageFormat]::Png)
$mask = New-Object Drawing.Bitmap $S, $S; $mg = [Drawing.Graphics]::FromImage($mask); $mg.Clear([Drawing.Color]::Black)
$m0 = [int](($off + $Border) * $k); $m1 = [int](($off + $T - $Border) * $k)
$mc = ($m0 + $m1) / 2
$mg.FillPolygon([Drawing.Brushes]::White, [Drawing.Point[]]@((New-Object Drawing.Point $mc, $m0), (New-Object Drawing.Point $m1, $mc), (New-Object Drawing.Point $mc, $m1), (New-Object Drawing.Point $m0, $mc))); $mg.Dispose()
$maskPath = Join-Path $work "$Name-mask.png"; $mask.Save($maskPath, [Drawing.Imaging.ImageFormat]::Png); $mask.Dispose()
$raw = Join-Path $work "$Name-raw.png"
$ErrorActionPreference = "Continue"
if (-not ($Reuse -and (Test-Path $raw))) { & $Sd -m $Model --vae $Vae -p $Prompt -n $Negative -i $initPath --mask $maskPath --strength 1.0 -W $S -H $S `
    --steps $Steps --cfg-scale $Cfg --sampling-method dpm++2m -s $Seed --vae-tiling -o $raw 2>&1 | Out-Null }
$ErrorActionPreference = "Stop"
if (-not (Test-Path $raw)) { throw "no image" }
# Only the subject (whatever differs from the painting's own grass, inside the tile's diamond)
# is laid onto the original tile's grass.
Add-Type -Path (Join-Path $PSScriptRoot "ImageOps.cs") -ReferencedAssemblies System.Drawing
$gen = [Drawing.Bitmap]::FromFile($raw)
$result = New-Object Drawing.Bitmap $T, $T, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
$tg = [Drawing.Graphics]::FromImage($result); $tg.InterpolationMode = 'HighQualityBicubic'; $tg.PixelOffsetMode = 'HighQuality'
$tg.DrawImage($gen, (New-Object Drawing.Rectangle 0, 0, $T, $T), (New-Object Drawing.Rectangle ([int]($off * $k)), ([int]($off * $k)), ([int]($T * $k)), ([int]($T * $k))), 'Pixel')
$tg.Dispose(); $gen.Dispose()
$gp = [ImageOps]::Read($result); $result.Dispose()
$orig = [Drawing.Bitmap]::FromFile((Resolve-Path (Join-Path $assets "x2\MapTiles\$Tile.png")))
$op = [ImageOps]::Read($orig); $orig.Dispose()
$mask = [ImageOps]::SubjectMask($gp, $T, $T, [ImageOps]::GrassColour($gp, $T, $T), $Tolerance)
$half = $T / 2 - $Border - 2
for ($y = 0; $y -lt $T; $y++) { for ($x = 0; $x -lt $T; $x++) {
    if ([Math]::Abs($x - ($T - 1) / 2) + [Math]::Abs($y - ($T - 1) / 2) -gt $half) { $mask[$y * $T + $x] = $false } } }
if ($Open -gt 0) { $mask = [ImageOps]::OpenMask($mask, $T, $T, $Open) }   # thin leftovers (a lawn's edge line) go
[ImageOps]::PasteMasked($op, $gp, $mask, $T, $T)
$gp = $op
$final = [ImageOps]::Write($gp, $T, $T)
$final.Save((Join-Path $out "$Name.png"), [Drawing.Imaging.ImageFormat]::Png); $final.Dispose()
"made $out\$Name.png"
