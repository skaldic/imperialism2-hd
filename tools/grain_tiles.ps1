# Puts the original tiles' fine grain back into the 2x terrain tiles, so grassland and the other
# repeating terrain look less smooth. The 2x tiles as upscaled are kept in assets\x2_before_grain
# and every run starts again from them, so it can be repeated with another -Amount.
#
# Green tiles also get soft light and dark patches (ImageOps.Mottle). Every tile has the same
# patch pattern, so neighbouring tiles of different kinds still meet without a seam.
#
#   .\grain_tiles.ps1 -Amount 1.0 -Patches 0.10
param(
    [double]$Amount = 1.0,
    [double]$Patches = 0.10,
    [string[]]$Terrain = @('MapTiles'),
    [string]$Assets = (Join-Path $PSScriptRoot "..\assets")
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -Path (Join-Path $PSScriptRoot "ImageOps.cs") -ReferencedAssemblies System.Drawing
$Assets = (Resolve-Path $Assets).Path
$done = 0
foreach ($dir in $Terrain) {
    $backup = "$Assets\x2_before_grain\$dir"
    New-Item -ItemType Directory -Force $backup | Out-Null
    foreach ($file in Get-ChildItem -LiteralPath "$Assets\orig\$dir" -Filter *.png) {
        $hdPath = "$Assets\x2\$dir\$($file.Name)"; $keep = "$backup\$($file.Name)"
        if (-not (Test-Path -LiteralPath $hdPath)) { continue }
        $o = [Drawing.Bitmap]::FromFile($file.FullName)
        $w = $o.Width; $h = $o.Height; $op = [ImageOps]::Read($o); $o.Dispose()
        # Only key-less 64x64 pictures repeat as terrain; sprites keep their magenta background.
        if ($w -ne 64 -or $h -ne 64 -or ($op | Where-Object { ($_ -band 0xffffff) -eq 0xff00ff } | Select-Object -First 1)) { continue }
        if (-not (Test-Path -LiteralPath $keep)) { Copy-Item -LiteralPath $hdPath $keep }
        $b = [Drawing.Bitmap]::FromFile($keep)
        $w2 = $b.Width; $h2 = $b.Height; $hp = [ImageOps]::Read($b); $b.Dispose()
        [ImageOps]::AddGrain($hp, $w2, $h2, $op, $w, $h, $Amount)
        $r = 0; $g = 0; $bl = 0
        foreach ($q in $op) { $r += ($q -shr 16) -band 255; $g += ($q -shr 8) -band 255; $bl += $q -band 255 }
        $n = $op.Length; $green = $g / $n -gt $r / $n + 15 -and $g / $n -gt $bl / $n + 15
        if ($Patches -gt 0 -and $green) { [ImageOps]::Mottle($hp, $w2, $h2, $Patches, 7) }
        $outBmp = [ImageOps]::Write($hp, $w2, $h2); $outBmp.Save($hdPath, [Drawing.Imaging.ImageFormat]::Png); $outBmp.Dispose()
        $done++
    }
}
"grain $Amount (patches $Patches on green tiles) added to $done tiles"
