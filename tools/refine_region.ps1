# Repaints one region of a painted-out picture in more detail: the region is enlarged, redrawn by the
# image model at a moderate strength (so the composition stays), shrunk back and pasted with a
# soft edge into both the 2x picture (assets\x2\_wide) and the 1x one (assets\wide).
#
#   .\refine_region.ps1 -Id 4502 -X 918 -Y 172 -W 98 -H 218 -Prompt "..." -Seed 1,2,3
#
# Coordinates are in the 1x picture. With several seeds, candidates are written to
# assets\wide\_candidates; pass one seed with -Apply to paste it in.
param(
    [Parameter(Mandatory)][int]$Id,
    [Parameter(Mandatory)][int]$X, [Parameter(Mandatory)][int]$Y,
    [Parameter(Mandatory)][int]$W, [Parameter(Mandatory)][int]$H,
    [Parameter(Mandatory)][string]$Prompt,
    [string]$Negative = "blurry, deformed face, extra limbs, text, watermark, frame",
    [int[]]$Seed = @(1),
    [double]$Strength = 0.45,
    [int]$Steps = 36,
    [double]$Cfg = 6.5,
    [int]$Feather = 6,                            # 1x pixels of soft edge
    [switch]$Apply,
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
$work = Join-Path $assets "wide\_candidates"
$p1 = Join-Path $assets "wide\$Id.png"; $p2 = Join-Path $assets "x2\_wide\$Id.png"

function Resize($bmp, $w, $h) {
    $out = New-Object Drawing.Bitmap $w, $h, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [Drawing.Graphics]::FromImage($out)
    $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
    $g.DrawImage($bmp, 0, 0, $w, $h); $g.Dispose(); $out
}
function Load($path) { $b = [Drawing.Bitmap]::FromFile($path); $c = New-Object Drawing.Bitmap $b; $b.Dispose(); $c }

# The region at 2x, enlarged to about a million pixels in multiples of 64.
$big = Load $p2
$crop = $big.Clone((New-Object Drawing.Rectangle ($X * 2), ($Y * 2), ($W * 2), ($H * 2)), [Drawing.Imaging.PixelFormat]::Format24bppRgb)
$scale = [Math]::Sqrt(1048576.0 / ($W * 2 * $H * 2))
$GW = [int]([Math]::Round($W * 2 * $scale / 64) * 64); $GH = [int]([Math]::Round($H * 2 * $scale / 64) * 64)

foreach ($s in $Seed) {
    $out = Join-Path $work "$Id-region-$X-$Y-seed$s.png"
    if (-not $Apply -or -not (Test-Path $out)) {
        $init = Resize $crop $GW $GH; $initPath = Join-Path $work "$Id-region-init.png"
        $init.Save($initPath, [Drawing.Imaging.ImageFormat]::Png); $init.Dispose()
        $mask = New-Object Drawing.Bitmap $GW, $GH; $mg = [Drawing.Graphics]::FromImage($mask)
        $mg.Clear([Drawing.Color]::White); $mg.Dispose()
        $maskPath = Join-Path $work "$Id-region-mask.png"; $mask.Save($maskPath, [Drawing.Imaging.ImageFormat]::Png); $mask.Dispose()
        $ErrorActionPreference = "Continue"
        & $Sd -m $Model --vae $Vae -p $Prompt -n $Negative -i $initPath --mask $maskPath --strength $Strength `
            -W $GW -H $GH --steps $Steps --cfg-scale $Cfg --sampling-method dpm++2m -s $s --vae-tiling -o $out 2>&1 | Out-Null
        $ErrorActionPreference = "Stop"
        if (-not (Test-Path $out)) { throw "no image for seed $s" }
        "candidate: $out"
    }
    if (-not $Apply) { continue }

    # Paste into both pictures with a soft edge.
    $gen = Load $out
    foreach ($target in @(@($p2, 2), @($p1, 1))) {
        $k = $target[1]; $pic = Load $target[0]
        $piece = Resize $gen ($W * $k) ($H * $k)
        $f = $Feather * $k
        for ($yy = 0; $yy -lt $H * $k; $yy++) {
            for ($xx = 0; $xx -lt $W * $k; $xx++) {
                $d = [Math]::Min([Math]::Min($xx, $W * $k - 1 - $xx), [Math]::Min($yy, $H * $k - 1 - $yy))
                $t = [Math]::Min(1.0, ($d + 0.5) / $f)
                $a = $pic.GetPixel($X * $k + $xx, $Y * $k + $yy); $b = $piece.GetPixel($xx, $yy)
                $pic.SetPixel($X * $k + $xx, $Y * $k + $yy, [Drawing.Color]::FromArgb(
                    [int]($a.R + ($b.R - $a.R) * $t), [int]($a.G + ($b.G - $a.G) * $t), [int]($a.B + ($b.B - $a.B) * $t)))
            }
        }
        $pic.Save($target[0], [Drawing.Imaging.ImageFormat]::Png); $pic.Dispose(); $piece.Dispose()
    }
    $gen.Dispose()
    "pasted seed $s into $p1 and its 2x"
    break
}
