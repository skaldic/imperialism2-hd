# Extracts every picture from the game's .rsrc archives to PNG: <OutDir>\<archive>\<id>.png.
#
# Archive layout: "rsrc", version, header size (16), entry count, then one 16-byte entry per
# resource: type (four characters, byte-reversed), id, offset, size. A 'PICT' resource is a Windows
# DIB (BITMAPINFOHEADER, colour table, pixels) without the 14-byte file header.
param(
    [string]$DataDir = "$PSScriptRoot\..\..\Data",
    [string]$OutDir = "$PSScriptRoot\..\assets\orig"
)

Add-Type -AssemblyName System.Drawing
$count = 0
foreach ($file in Get-ChildItem -LiteralPath $DataDir -Filter *.rsrc) {
    $b = [IO.File]::ReadAllBytes($file.FullName)
    if ([Text.Encoding]::ASCII.GetString($b, 0, 4) -ne 'rsrc') { continue }
    $dir = Join-Path $OutDir $file.BaseName
    $entries = [BitConverter]::ToInt32($b, 12)
    for ($i = 0; $i -lt $entries; $i++) {
        $e = 16 + 16 * $i
        $type = [Text.Encoding]::ASCII.GetString(@($b[$e + 3], $b[$e + 2], $b[$e + 1], $b[$e]))
        $id = [BitConverter]::ToInt32($b, $e + 4)
        $offset = [BitConverter]::ToInt32($b, $e + 8)
        $size = [BitConverter]::ToInt32($b, $e + 12)
        if ($type -ne 'PICT' -or [BitConverter]::ToInt32($b, $offset) -ne 40) { continue }

        $bpp = [BitConverter]::ToUInt16($b, $offset + 14)
        $colours = [BitConverter]::ToInt32($b, $offset + 32)
        if ($bpp -le 8 -and $colours -eq 0) { $colours = 1 -shl $bpp }

        # Prepend a BMP file header so GDI+ can read it.
        $ms = New-Object IO.MemoryStream
        $w = New-Object IO.BinaryWriter($ms)
        $w.Write([byte[]](0x42, 0x4D)); $w.Write([int](14 + $size)); $w.Write([int]0)
        $w.Write([int](14 + 40 + 4 * $colours))
        $w.Write($b, $offset, $size)
        $ms.Position = 0
        $bmp = New-Object Drawing.Bitmap($ms)
        if ($count -eq 0 -or -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
        $bmp.Save((Join-Path $dir "$id.png"), [Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose(); $ms.Dispose()
        $count++
    }
}
"$count pictures -> $OutDir"
