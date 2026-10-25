# Repaints vertical strips of an already widened picture with the image model, for insertions that
# stretching cannot fill well (the diplomacy screens' sea chart). Each strip is painted as a 2x
# close-up with the picture around it as context, then joined back with a soft edge. Writes
# assets\wide\<id>.png and its 2x upscale assets\x2\_wide\<id>.png, for 'G' insertions.
#
#   .\inpaint_strips.ps1 -Base wide4140.png -Id 4140 -Strips "100,245,125","830,245,131" -Prompt "..."
#
# -Base is the picture at the widened size with the strips filled any way (they are repainted).
# Each strip is "x,top,width" in it, running to the bottom.
param(
    [Parameter(Mandatory)][string]$Base,
    [Parameter(Mandatory)][int]$Id,
    [Parameter(Mandatory)][string[]]$Strips,
    [Parameter(Mandatory)][string]$Prompt,
    [string]$Negative = "text, letters, watermark, signature, people, photo, blurry, frame",
    [int]$Seed = 1,
    [int]$Steps = 32,
    [double]$Cfg = 6.5,
    [int]$Feather = 12,
    [int]$Overlap = 10,                           # columns of the picture repainted beside each strip
    [string]$Sd = (Join-Path (& "$PSScriptRoot\tools_dir.ps1") "sdcpp\bin\sd-cli.exe"),
    [string]$Model = (Join-Path (& "$PSScriptRoot\tools_dir.ps1") "sdcpp\models\sdxl_inpaint.safetensors"),
    [string]$Vae = (Join-Path (& "$PSScriptRoot\tools_dir.ps1") "sdcpp\models\sdxl_vae_fp16_fix.safetensors"),
    [string]$Upscaler = (Join-Path (& "$PSScriptRoot\tools_dir.ps1") "realesrgan\realesrgan-ncnn-vulkan.exe")
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$assets = Join-Path $PSScriptRoot "..\assets"
$work = Join-Path $assets "wide\_candidates"
New-Item -ItemType Directory -Force $work, (Join-Path $assets "x2\_wide") | Out-Null

function Resize($bmp, $w, $h) {
    $out = New-Object Drawing.Bitmap $w, $h, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [Drawing.Graphics]::FromImage($out)
    $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
    $g.DrawImage($bmp, 0, 0, $w, $h); $g.Dispose(); $out
}
$b = [Drawing.Bitmap]::FromFile((Resolve-Path $Base)); $pic = New-Object Drawing.Bitmap $b; $b.Dispose()
$W = $pic.Width; $H = $pic.Height

foreach ($strip in $Strips) {
    $sx, $top, $sw = [int[]]($strip -split ',')
    # A 512-wide close-up centred on the strip, 384 rows ending at the bottom (2x: 1024x768).
    $cw = 512; $ch = 384
    $cx = [Math]::Max(0, [Math]::Min($W - $cw, $sx + [int]($sw / 2) - [int]($cw / 2)))
    $cy = $H - $ch
    $crop = $pic.Clone((New-Object Drawing.Rectangle $cx, $cy, $cw, $ch), [Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $init = Resize $crop ($cw * 2) ($ch * 2); $crop.Dispose()
    $initPath = Join-Path $work "$Id-strip-init.png"; $init.Save($initPath, [Drawing.Imaging.ImageFormat]::Png); $init.Dispose()
    $mask = New-Object Drawing.Bitmap ($cw * 2), ($ch * 2); $mg = [Drawing.Graphics]::FromImage($mask); $mg.Clear([Drawing.Color]::Black)
    $mx = ($sx - $cx - $Overlap) * 2; $my = ($top - $cy - 2) * 2
    $mg.FillRectangle([Drawing.Brushes]::White, $mx, $my, ($sw + 2 * $Overlap) * 2, $ch * 2 - $my); $mg.Dispose()
    $maskPath = Join-Path $work "$Id-strip-mask.png"; $mask.Save($maskPath, [Drawing.Imaging.ImageFormat]::Png); $mask.Dispose()
    $out = Join-Path $work "$Id-strip-$sx-seed$Seed.png"
    $ErrorActionPreference = "Continue"
    & $Sd -m $Model --vae $Vae -p $Prompt -n $Negative -i $initPath --mask $maskPath --strength 1.0 `
        -W ($cw * 2) -H ($ch * 2) --steps $Steps --cfg-scale $Cfg --sampling-method dpm++2m -s $Seed --vae-tiling -o $out 2>&1 | Out-Null
    $ErrorActionPreference = "Stop"
    if (-not (Test-Path $out)) { throw "no image for strip at $sx" }
    $gb = [Drawing.Bitmap]::FromFile($out); $gen = Resize $gb $cw $ch; $gb.Dispose()
    # Paste the strip (and 4 columns either side) back, feathered at its edges.
    for ($y = $top - 2; $y -lt $H; $y++) {
        for ($x = $sx - $Overlap; $x -lt $sx + $sw + $Overlap; $x++) {
            $d = [Math]::Min([Math]::Min($x - ($sx - $Overlap), ($sx + $sw + $Overlap - 1) - $x), $y - ($top - 2))
            $t = [Math]::Min(1.0, ($d + 0.5) / $Feather)
            $a = $pic.GetPixel($x, $y); $c = $gen.GetPixel($x - $cx, $y - $cy)
            $pic.SetPixel($x, $y, [Drawing.Color]::FromArgb([int]($a.R + ($c.R - $a.R) * $t), [int]($a.G + ($c.G - $a.G) * $t), [int]($a.B + ($c.B - $a.B) * $t)))
        }
    }
    $gen.Dispose()
    "painted strip at $sx"
}
$final = Join-Path $assets "wide\$Id.png"
$pic.Save($final, [Drawing.Imaging.ImageFormat]::Png); $pic.Dispose()
$tmp4 = Join-Path $work "$Id-x4.png"
$ErrorActionPreference = "Continue"
& $Upscaler -i $final -o $tmp4 -n realesrgan-x4plus -s 4 -f png 2>&1 | Out-Null
$ErrorActionPreference = "Stop"
$x4 = [Drawing.Bitmap]::FromFile($tmp4); $x2 = Resize $x4 ($W * 2) ($H * 2); $x4.Dispose()
$x2.Save((Join-Path $assets "x2\_wide\$Id.png"), [Drawing.Imaging.ImageFormat]::Png); $x2.Dispose(); Remove-Item $tmp4
"installed $final and its 2x"
