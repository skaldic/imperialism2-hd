# Paints out an 800x600 screen picture to the wide width (1056x600) with an inpainting image
# model, for layouts with 'G' insertions (hd\layouts.h). The picture is kept whole in the middle;
# the model paints the 128 columns either side. Writes assets\wide\<id>.png and its 2x upscale
# assets\x2\_wide\<id>.png.
#
#   .\outpaint.ps1 -Picture Embedded\4502 -Prompt "an ornate 17th century study ..."
#
# Needs stable-diffusion.cpp (sd-cli) and an SDXL inpainting checkpoint; several seeds can be
# tried with -Seed and compared in assets\wide\_candidates.
param(
    [Parameter(Mandatory)][string]$Picture,       # archive\id under assets\orig
    [Parameter(Mandatory)][string]$Prompt,
    [string]$Negative = "text, letters, watermark, signature, frame border, blurry, people, photo",
    [int[]]$Seed = @(1),
    [int]$Steps = 32,
    [double]$Cfg = 6.5,
    [int]$Overlap = 12,                           # columns of the original repainted at each seam
    [int]$Blend = 12,                             # of those, crossfaded into the original (GENERATED_BLEND)
    [string]$Sd = "D:\Tools\sdcpp\bin\sd-cli.exe",
    [string]$Model = "D:\Tools\sdcpp\models\sdxl_inpaint.safetensors",
    [string]$Vae = "D:\Tools\sdcpp\models\sdxl_vae_fp16_fix.safetensors",
    [string]$Upscaler = "D:\Tools\realesrgan\realesrgan-ncnn-vulkan.exe",
    [int]$Pick = 0                                # with a seed: make that candidate the one used
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$assets = Join-Path $PSScriptRoot "..\assets"
$id = Split-Path $Picture -Leaf
$work = Join-Path $assets "wide\_candidates"
New-Item -ItemType Directory -Force $work, (Join-Path $assets "x2\_wide") | Out-Null

$W = 1056; $H = 600; $M = 128
# The model works best near a million pixels, in multiples of 64.
$GW = 1344; $GH = 768

function Save-Png($bmp, $path) { $bmp.Save($path, [Drawing.Imaging.ImageFormat]::Png) }
function Resize($bmp, $w, $h) {
    $out = New-Object Drawing.Bitmap $w, $h, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [Drawing.Graphics]::FromImage($out)
    $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
    $g.DrawImage($bmp, 0, 0, $w, $h); $g.Dispose(); $out
}

$orig = [Drawing.Bitmap]::FromFile((Resolve-Path (Join-Path $assets "orig\$Picture.png")))
if ($orig.Width -ne 800 -or $orig.Height -ne $H) { throw "expected an 800x600 picture" }

if ($Pick -eq 0) {
    # The starting picture: the original in the middle, the margins filled with its mirrored edges,
    # blurred, as a hint of colour and light for the model.
    $canvas = New-Object Drawing.Bitmap $W, $H, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [Drawing.Graphics]::FromImage($canvas)
    $g.DrawImage($orig, $M, 0, 800, $H)
    $left = New-Object Drawing.Bitmap $M, $H; $lg = [Drawing.Graphics]::FromImage($left)
    $lg.DrawImage($orig, (New-Object Drawing.Rectangle 0, 0, $M, $H), (New-Object Drawing.Rectangle 0, 0, $M, $H), 'Pixel')
    $left.RotateFlip('RotateNoneFlipX')
    $right = New-Object Drawing.Bitmap $M, $H; $rg = [Drawing.Graphics]::FromImage($right)
    $rg.DrawImage($orig, (New-Object Drawing.Rectangle 0, 0, $M, $H), (New-Object Drawing.Rectangle (800 - $M), 0, $M, $H), 'Pixel')
    $right.RotateFlip('RotateNoneFlipX')
    foreach ($side in @(@($left, 0), @($right, ($W - $M)))) {
        $small = Resize $side[0] ([int]($M / 8)) ([int]($H / 8))
        $g.InterpolationMode = 'HighQualityBicubic'
        $g.DrawImage($small, $side[1], 0, $M, $H)
    }
    $g.Dispose()
    # Each side is painted on its own, as a close-up: the 128-column margin and 384 columns of the
    # original beside it, at twice the size. At that scale the model sees the objects cut by the
    # edge in detail and carries them on, instead of starting something new.
    $CW = 512; $GW = $CW * 2; $GH = $H * 2
    foreach ($s in $Seed) {
        $wide = New-Object Drawing.Bitmap $W, $H, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
        $wg = [Drawing.Graphics]::FromImage($wide); $wg.DrawImage($canvas, 0, 0, $W, $H)
        foreach ($side in 'left', 'right') {
            $cx = if ($side -eq 'left') { 0 } else { $W - $CW }
            $crop = New-Object Drawing.Bitmap $CW, $H, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
            $cg = [Drawing.Graphics]::FromImage($crop)
            $cg.DrawImage($canvas, (New-Object Drawing.Rectangle 0, 0, $CW, $H), (New-Object Drawing.Rectangle $cx, 0, $CW, $H), 'Pixel'); $cg.Dispose()
            $init = Resize $crop $GW $GH; $crop.Dispose()
            $initPath = Join-Path $work "$id-$side-init.png"; Save-Png $init $initPath; $init.Dispose()
            $mask = New-Object Drawing.Bitmap $GW, $GH, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
            $mg = [Drawing.Graphics]::FromImage($mask); $mg.Clear([Drawing.Color]::Black)
            $mw = ($M + $Overlap) * 2
            $mx = if ($side -eq 'left') { 0 } else { $GW - $mw }
            $mg.FillRectangle([Drawing.Brushes]::White, $mx, 0, $mw, $GH); $mg.Dispose()
            $maskPath = Join-Path $work "$id-$side-mask.png"; Save-Png $mask $maskPath; $mask.Dispose()

            $outPath = Join-Path $work "$id-seed$s-$side-raw.png"
            $ErrorActionPreference = "Continue" # sd-cli reports progress on stderr
            & $Sd -m $Model --vae $Vae -p $Prompt -n $Negative -i $initPath --mask $maskPath --strength 1.0 `
                -W $GW -H $GH --steps $Steps --cfg-scale $Cfg --sampling-method dpm++2m -s $s --vae-tiling -o $outPath 2>&1 |
                ForEach-Object { "$_" } | Select-String -Pattern 'error|fail' | ForEach-Object Line
            $ErrorActionPreference = "Stop"
            if (-not (Test-Path $outPath)) { throw "no image for seed $s ($side)" }
            $gen = [Drawing.Bitmap]::FromFile($outPath)
            $back = Resize $gen $CW $H; $gen.Dispose()
            $wg.DrawImage($back, $cx, 0, $CW, $H); $back.Dispose()
        }
        $wg.Dispose()        # The original goes back over the middle, crossfaded into the model's version over the
        # $Blend columns next to each margin (the patch draws those columns from this picture too).
        $wg = [Drawing.Graphics]::FromImage($wide)
        $wg.DrawImage($orig, (New-Object Drawing.Rectangle ($M + $Blend), 0, (800 - 2 * $Blend), $H), (New-Object Drawing.Rectangle $Blend, 0, (800 - 2 * $Blend), $H), 'Pixel')
        $wg.Dispose()
        # In the $Blend columns next to each margin, each row switches from one version to the
        # other where they differ least (a seam found by dynamic programming, so it does not jump
        # between rows), with a narrow soft edge. A plain crossfade would double-expose anything
        # the model repainted slightly differently.
        foreach ($side in 'left', 'right') {
            $x0 = if ($side -eq 'left') { $M } else { $W - $M - $Blend }
            $cost = New-Object 'double[,]' $H, $Blend
            for ($y = 0; $y -lt $H; $y++) {
                for ($k = 0; $k -lt $Blend; $k++) {
                    $a = $wide.GetPixel($x0 + $k, $y); $o = $orig.GetPixel($x0 + $k - $M, $y)
                    $d = [Math]::Abs($a.R - $o.R) + [Math]::Abs($a.G - $o.G) + [Math]::Abs($a.B - $o.B)
                    if ($y -gt 0) {
                        $best = $cost[($y - 1), $k]
                        if ($k -gt 0 -and $cost[($y - 1), ($k - 1)] -lt $best) { $best = $cost[($y - 1), ($k - 1)] }
                        if ($k -lt $Blend - 1 -and $cost[($y - 1), ($k + 1)] -lt $best) { $best = $cost[($y - 1), ($k + 1)] }
                        $d += $best
                    }
                    $cost[$y, $k] = $d
                }
            }
            $seam = New-Object 'int[]' $H
            $k = 0; for ($j = 1; $j -lt $Blend; $j++) { if ($cost[($H - 1), $j] -lt $cost[($H - 1), $k]) { $k = $j } }
            for ($y = $H - 1; $y -ge 0; $y--) {
                $seam[$y] = $k
                if ($y -gt 0) {
                    $next = $k
                    if ($k -gt 0 -and $cost[($y - 1), ($k - 1)] -lt $cost[($y - 1), $next]) { $next = $k - 1 }
                    if ($k -lt $Blend - 1 -and $cost[($y - 1), ($k + 1)] -lt $cost[($y - 1), $next]) { $next = $k + 1 }
                    $k = $next
                }
            }
            for ($y = 0; $y -lt $H; $y++) {
                for ($k = 0; $k -lt $Blend; $k++) {
                    # share of the original: 0 on the margin's side of the seam, 1 on the other
                    $t = [Math]::Min(1, [Math]::Max(0, (($k - $seam[$y]) / 3.0) + 0.5))
                    if ($side -eq 'right') { $t = 1 - $t }
                    if ($t -le 0) { continue }
                    $a = $wide.GetPixel($x0 + $k, $y); $o = $orig.GetPixel($x0 + $k - $M, $y)
                    $wide.SetPixel($x0 + $k, $y, [Drawing.Color]::FromArgb(
                        [int]($a.R + ($o.R - $a.R) * $t), [int]($a.G + ($o.G - $a.G) * $t), [int]($a.B + ($o.B - $a.B) * $t)))
                }
            }
        }
        Save-Png $wide (Join-Path $work "$id-seed$s.png"); $wide.Dispose()
        "candidate: $work\$id-seed$s.png"
    }
    if ($Seed.Count -gt 1) { "pick one with -Seed <n> -Pick 1"; return }
}

# Make the chosen candidate the one used, and upscale it.
$chosen = Join-Path $work "$id-seed$($Seed[0]).png"
$final = Join-Path $assets "wide\$id.png"
Copy-Item $chosen $final -Force
$tmp4 = Join-Path $work "$id-x4.png"
$ErrorActionPreference = "Continue"
& $Upscaler -i $final -o $tmp4 -n realesrgan-x4plus -s 4 -f png 2>&1 | Out-Null
$ErrorActionPreference = "Stop"
$x4 = [Drawing.Bitmap]::FromFile($tmp4)
$x2 = Resize $x4 ($W * 2) ($H * 2); $x4.Dispose()
Save-Png $x2 (Join-Path $assets "x2\_wide\$id.png"); $x2.Dispose()
Remove-Item $tmp4
"installed $final and its 2x"
