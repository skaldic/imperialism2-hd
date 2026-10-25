# Builds the release archive: dist\ImperialismII-HD-<version>.zip, to be unzipped into the game's
# folder. It holds the DLL (built fresh), a default ini, and the HD art under imp2hd\:
#   x2\hd.pack                  map tiles and small pictures (tools\build_pack.ps1)
#   x2\<archive>\<id>.png       pictures too large for the pack (full screens)
#   x2\_wide, x4\_wide, wide    painted-out screens: only the finished <id>.png of each
# plus the readme for players, the license and the notice. Work files are left out.
#
#   .\make_release.ps1 -Version 1.0.0
param(
    [Parameter(Mandatory)][string]$Version,
    [string]$Assets = "",      # default: assets next to the tools folder
    [int]$PackMaxSide = 512    # must match build_pack.ps1: larger pictures ship as PNG
)
$ErrorActionPreference = "Stop"
if (-not $Assets) { $Assets = Join-Path $PSScriptRoot "..\assets" }
Add-Type -AssemblyName System.Drawing, System.IO.Compression.FileSystem
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Assets = (Resolve-Path $Assets).Path
$stage = Join-Path $root "dist\stage"
$zip = Join-Path $root "dist\ImperialismII-HD-$Version.zip"
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\imp2hd\x2" | Out-Null

& "$root\hd\build.ps1"
Copy-Item "$root\hd\ddraw.dll" $stage
Set-Content -Encoding ascii "$stage\imp2hd.ini" "[Display]`r`nWide=1`r`nHD=1`r`n"
Copy-Item "$root\LICENSE.md", "$root\NOTICE.md" "$stage\imp2hd"
Copy-Item "$root\INSTALL.txt" "$stage\imp2hd\README.txt"

if (-not (Test-Path "$Assets\x2\hd.pack")) { throw "no x2\hd.pack: run tools\build_pack.ps1 first" }
Copy-Item "$Assets\x2\hd.pack" "$stage\imp2hd\x2"
$large = 0
foreach ($dir in Get-ChildItem "$Assets\x2" -Directory | Where-Object { $_.Name -notlike '_*' }) {
    foreach ($file in Get-ChildItem $dir.FullName -Filter *.png) {
        $b = [Drawing.Bitmap]::FromFile($file.FullName); $w = $b.Width; $h = $b.Height; $b.Dispose()
        if ($w -le $PackMaxSide -and $h -le $PackMaxSide) { continue }
        New-Item -ItemType Directory -Force "$stage\imp2hd\x2\$($dir.Name)" | Out-Null
        Copy-Item $file.FullName "$stage\imp2hd\x2\$($dir.Name)"
        $large++
    }
}
foreach ($set in 'x2\_wide', 'x4\_wide', 'wide') {
    if (-not (Test-Path "$Assets\$set")) { continue }
    New-Item -ItemType Directory -Force "$stage\imp2hd\$set" | Out-Null
    Get-ChildItem "$Assets\$set" -File -Filter *.png | Where-Object { $_.BaseName -match '^\d+$' } |
        Copy-Item -Destination "$stage\imp2hd\$set"
}

Remove-Item $zip -ErrorAction SilentlyContinue
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
"$large large pictures; $zip ($([int]((Get-Item $zip).Length / 1MB)) MB)"
