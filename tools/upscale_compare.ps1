# Builds a side-by-side comparison of 2x upscaling methods on a handful of representative pictures.
# Columns: original (pixel-doubled), bicubic, Real-ESRGAN animevideov3 x2, Real-ESRGAN x4plus
# reduced to 2x. Needs the pictures from extract_rsrc.ps1 and realesrgan-ncnn-vulkan.
param(
    [string]$Assets = "$PSScriptRoot\..\assets",
    [string]$Upscaler = (Join-Path (& "$PSScriptRoot\tools_dir.ps1") "realesrgan\realesrgan-ncnn-vulkan.exe")
)

Add-Type -AssemblyName System.Drawing
$work = "$Assets\compare"
$in = "$work\in"
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $in, "$work\anime2", "$work\plus4" | Out-Null

# name -> source pictures (several are placed side by side) and the region to show, in source pixels
$samples = @(
    @{ Name = 'background'; Files = @('Startup\4200');  Crop = @(300, 150, 200, 150) }
    @{ Name = 'portrait';   Files = @('Minister\9525'); Crop = @(0, 15, 180, 135) }
    @{ Name = 'city';       Files = @('CityMain\9198'); Crop = @(200, 100, 200, 150) }
    @{ Name = 'battle';     Files = @('Tactical\3825'); Crop = @(100, 80, 200, 150) }
    @{ Name = 'tiles';      Files = @('MapTiles\10008', 'MapTiles\10015', 'MapTiles\10003'); Crop = @(0, 0, 192, 64) }
    @{ Name = 'panel';      Files = @('MapView\4122');  Crop = @(50, 150, 200, 150) }
)

foreach ($s in $samples) {
    $images = $s.Files | ForEach-Object { [Drawing.Image]::FromFile("$Assets\orig\$_.png") }
    $w = ($images | Measure-Object Width -Sum).Sum
    $h = ($images | Measure-Object Height -Maximum).Maximum
    $bmp = New-Object Drawing.Bitmap($w, $h, [Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $x = 0
    foreach ($img in $images) { $g.DrawImage($img, $x, 0, $img.Width, $img.Height); $x += $img.Width; $img.Dispose() }
    $g.Dispose()
    $bmp.Save("$in\$($s.Name).png", [Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

& $Upscaler -i $in -o "$work\anime2" -n realesr-animevideov3 -s 2 -f png 2>$null
& $Upscaler -i $in -o "$work\plus4" -n realesrgan-x4plus -s 4 -f png 2>$null

$columns = 'original (pixel-doubled)', 'bicubic 2x', 'Real-ESRGAN animevideov3 2x', 'Real-ESRGAN x4plus -> 2x'
$cellW = 400; $cellH = 300; $header = 28
$sheet = New-Object Drawing.Bitmap(($cellW * 4), ($header + $cellH * $samples.Count))
$g = [Drawing.Graphics]::FromImage($sheet)
$g.Clear([Drawing.Color]::Black)
$font = New-Object Drawing.Font('Segoe UI', 11)
for ($c = 0; $c -lt 4; $c++) { $g.DrawString($columns[$c], $font, [Drawing.Brushes]::White, ($c * $cellW + 6), 4) }

function Draw-Cell($image, [int[]]$crop, [int]$scale, $mode, [int]$col, [int]$row) {
    $src = New-Object Drawing.Rectangle(($crop[0] * $scale), ($crop[1] * $scale), ($crop[2] * $scale), ($crop[3] * $scale))
    $dst = New-Object Drawing.Rectangle(($col * $cellW), ($header + $row * $cellH), ($crop[2] * 2), ($crop[3] * 2))
    $g.InterpolationMode = $mode
    $g.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::Half
    $g.DrawImage($image, $dst, $src, [Drawing.GraphicsUnit]::Pixel)
}

for ($r = 0; $r -lt $samples.Count; $r++) {
    $s = $samples[$r]
    $orig = [Drawing.Image]::FromFile("$in\$($s.Name).png")
    $anime = [Drawing.Image]::FromFile("$work\anime2\$($s.Name).png")
    $plus = [Drawing.Image]::FromFile("$work\plus4\$($s.Name).png")
    Draw-Cell $orig $s.Crop 1 ([Drawing.Drawing2D.InterpolationMode]::NearestNeighbor) 0 $r
    Draw-Cell $orig $s.Crop 1 ([Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic) 1 $r
    Draw-Cell $anime $s.Crop 2 ([Drawing.Drawing2D.InterpolationMode]::NearestNeighbor) 2 $r
    Draw-Cell $plus $s.Crop 4 ([Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic) 3 $r
    $orig.Dispose(); $anime.Dispose(); $plus.Dispose()
}
$g.Dispose()
$out = "$work\comparison.png"
$sheet.Save($out, [Drawing.Imaging.ImageFormat]::Png)
$sheet.Dispose()
"saved $out"
