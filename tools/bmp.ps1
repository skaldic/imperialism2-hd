# Converts an image between PNG and BMP (for hd\layouttool), by the output file's extension.
param([Parameter(Mandatory)][string]$In, [Parameter(Mandatory)][string]$Out)
Add-Type -AssemblyName System.Drawing
$img = [Drawing.Bitmap]::FromFile((Resolve-Path $In))
$copy = New-Object Drawing.Bitmap $img.Width, $img.Height, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
$g = [Drawing.Graphics]::FromImage($copy)
$g.DrawImage($img, 0, 0, $img.Width, $img.Height)
$g.Dispose(); $img.Dispose()
$format = if ($Out -match '\.bmp$') { [Drawing.Imaging.ImageFormat]::Bmp } else { [Drawing.Imaging.ImageFormat]::Png }
$copy.Save($Out, $format)
$copy.Dispose()
