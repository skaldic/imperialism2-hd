# Puts the original tiles' fine grain back into the 2x terrain, so grassland and the other
# terrain look less smooth: the game's own terrain pictures (MapTiles) and the map squares
# captured in play (_tiles, which is what the map is drawn from). The 2x versions as upscaled are
# kept in assets\x2_before_grain and every run starts again from them, so it can be repeated with
# another -Amount.
#
# Grass also gets soft light and dark patches (ImageOps.Mottle), each pixel by how green it is.
# Every square has the same patch pattern, so neighbouring squares still meet without a seam.
#
#   .\grain_tiles.ps1 -Amount 1.0 -Patches 0.10
#   .\grain_tiles.ps1 -NewOnly        only squares not done yet (upscale_all.ps1 runs this)
param(
    [double]$Amount = 1.0,
    [double]$Patches = 0.10,
    [string[]]$Terrain = @('MapTiles', '_tiles'),
    [string]$Assets = (Join-Path $PSScriptRoot "..\assets"),
    [switch]$NewOnly,
    [switch]$NoPack
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -Path (Join-Path $PSScriptRoot "ImageOps.cs") -ReferencedAssemblies System.Drawing
$Assets = (Resolve-Path $Assets).Path
$done = 0
foreach ($dir in $Terrain) {
    $backup = "$Assets\x2_before_grain\$dir"
    New-Item -ItemType Directory -Force $backup | Out-Null
    $wrap = $dir -ne '_tiles'   # the game's terrain pictures repeat; captured squares do not
    foreach ($file in Get-ChildItem -LiteralPath "$Assets\orig\$dir" -Filter *.png) {
        $hdPath = "$Assets\x2\$dir\$($file.Name)"; $keep = "$backup\$($file.Name)"
        if (-not (Test-Path -LiteralPath $hdPath)) { continue }
        if ($NewOnly -and (Test-Path -LiteralPath $keep)) { continue }
        $o = [Drawing.Bitmap]::FromFile($file.FullName)
        $w = $o.Width; $h = $o.Height; $op = [ImageOps]::Read($o); $o.Dispose()
        # Only key-less 64x64 pictures are terrain; sprites keep their magenta background.
        if ($w -ne 64 -or $h -ne 64 -or [ImageOps]::HasKey($op)) { continue }
        if (-not (Test-Path -LiteralPath $keep)) { Copy-Item -LiteralPath $hdPath $keep }
        $b = [Drawing.Bitmap]::FromFile($keep)
        $w2 = $b.Width; $h2 = $b.Height; $hp = [ImageOps]::Read($b); $b.Dispose()
        [ImageOps]::AddGrain($hp, $w2, $h2, $op, $w, $h, $Amount, $wrap)
        if ($Patches -gt 0) { [ImageOps]::Mottle($hp, $w2, $h2, $Patches, 7) }
        $outBmp = [ImageOps]::Write($hp, $w2, $h2); $outBmp.Save($hdPath, [Drawing.Imaging.ImageFormat]::Png); $outBmp.Dispose()
        $done++
    }
}
"grain $Amount (patches $Patches on grass) added to $done squares"
if ($done -gt 0 -and -not $NoPack) { & "$PSScriptRoot\build_pack.ps1" -Assets $Assets }
