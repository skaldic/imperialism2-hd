# Upscales the extracted pictures 2x: Real-ESRGAN x4plus at 4x, reduced to 2x.
#   assets\orig\<archive>\<id>.png  ->  assets\x2\<archive>\<id>.png
#
# Sprites mark transparent pixels with a key colour (magenta). Left in, the upscaler would smear
# it into the sprite's edges, so before upscaling the key area is filled with colours bled out
# from the sprite, and afterwards the key is stamped back from a pixel-doubled copy of the mask.
param(
    [string]$Assets = "$PSScriptRoot\..\assets",
    [string]$Upscaler = "D:\Tools\realesrgan\realesrgan-ncnn-vulkan.exe",
    [string[]]$Skip = @('MapTiles')   # tiles need seam-aware handling of their own
)

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class KeyedUpscale
{
    static bool IsKey(int argb)
    {
        int r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255;
        return r >= 240 && g <= 8 && b >= 240;
    }

    static int[] Read(Bitmap bmp)
    {
        var data = bmp.LockBits(new Rectangle(0, 0, bmp.Width, bmp.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        int[] px = new int[bmp.Width * bmp.Height];
        Marshal.Copy(data.Scan0, px, 0, px.Length);
        bmp.UnlockBits(data);
        return px;
    }

    static Bitmap Write(int[] px, int w, int h)
    {
        var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        var data = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        Marshal.Copy(px, 0, data.Scan0, px.Length);
        bmp.UnlockBits(data);
        return bmp;
    }

    // Writes the picture ready for the upscaler. Returns the transparency mask, or null if the
    // picture has no key-coloured pixels.
    public static bool[] Prepare(string source, string dest, out int width, out int height)
    {
        int[] px;
        using (var src = new Bitmap(source)) { width = src.Width; height = src.Height; px = Read(src); }
        int w = width, h = height;
        bool[] mask = new bool[px.Length];
        int keyed = 0;
        for (int i = 0; i < px.Length; i++) { mask[i] = IsKey(px[i]); if (mask[i]) keyed++; }

        if (keyed > 0 && keyed < px.Length)
        {
            // Grow the sprite's colours outward a few pixels at a time.
            bool[] open = (bool[])mask.Clone();
            for (int pass = 0; pass < 8; pass++)
            {
                int[] next = (int[])px.Clone();
                bool[] nextOpen = (bool[])open.Clone();
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                    {
                        int i = y * w + x;
                        if (!open[i]) continue;
                        int r = 0, g = 0, b = 0, n = 0;
                        for (int dy = -1; dy <= 1; dy++)
                            for (int dx = -1; dx <= 1; dx++)
                            {
                                int xx = x + dx, yy = y + dy;
                                if (xx < 0 || yy < 0 || xx >= w || yy >= h || open[yy * w + xx]) continue;
                                int c = px[yy * w + xx];
                                r += (c >> 16) & 255; g += (c >> 8) & 255; b += c & 255; n++;
                            }
                        if (n == 0) continue;
                        next[i] = unchecked((int)0xff000000) | ((r / n) << 16) | ((g / n) << 8) | (b / n);
                        nextOpen[i] = false;
                    }
                px = next; open = nextOpen;
            }
            for (int i = 0; i < px.Length; i++) if (open[i]) px[i] = unchecked((int)0xff000000);
        }
        using (var outBmp = Write(px, w, h)) outBmp.Save(dest, ImageFormat.Png);
        return keyed > 0 ? mask : null;
    }

    // Reduces the 4x result to 2x and stamps the key colour back.
    public static void Finish(string upscaled, string dest, bool[] mask, int width, int height)
    {
        int w2 = width * 2, h2 = height * 2;
        using (var big = new Bitmap(upscaled))
        using (var half = new Bitmap(w2, h2, PixelFormat.Format32bppArgb))
        {
            using (var g = Graphics.FromImage(half))
            {
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.PixelOffsetMode = PixelOffsetMode.Half;
                using (var attrs = new ImageAttributes())
                {
                    attrs.SetWrapMode(WrapMode.TileFlipXY); // no dark fringe at the picture's edges
                    g.DrawImage(big, new Rectangle(0, 0, w2, h2), 0, 0, big.Width, big.Height, GraphicsUnit.Pixel, attrs);
                }
            }
            if (mask == null) { Save24(half, dest); return; }
            int[] px = Read(half);
            for (int y = 0; y < h2; y++)
                for (int x = 0; x < w2; x++)
                    if (mask[(y / 2) * width + x / 2]) px[y * w2 + x] = unchecked((int)0xffff00ff);
            using (var keyed = Write(px, w2, h2)) Save24(keyed, dest);
        }
    }

    static void Save24(Bitmap bmp, string dest)
    {
        using (var rgb = bmp.Clone(new Rectangle(0, 0, bmp.Width, bmp.Height), PixelFormat.Format24bppRgb))
            rgb.Save(dest, ImageFormat.Png);
    }
}
'@

$orig = "$Assets\orig"
$tempIn = "$Assets\_in"
$tempOut = "$Assets\_out"
$total = 0; $keyed = 0
foreach ($dir in Get-ChildItem -LiteralPath $orig -Directory) {
    if ($Skip -contains $dir.Name) { "$($dir.Name): skipped"; continue }
    Remove-Item -Recurse -Force $tempIn, $tempOut -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $tempIn, $tempOut, "$Assets\x2\$($dir.Name)" | Out-Null

    $info = @{}
    foreach ($file in Get-ChildItem -LiteralPath $dir.FullName -Filter *.png) {
        $w = 0; $h = 0
        $mask = [KeyedUpscale]::Prepare($file.FullName, "$tempIn\$($file.Name)", [ref]$w, [ref]$h)
        $info[$file.Name] = @{ Mask = $mask; W = $w; H = $h }
    }
    & $Upscaler -i $tempIn -o $tempOut -n realesrgan-x4plus -s 4 -f png 2>$null

    $done = 0
    foreach ($name in $info.Keys) {
        if (-not (Test-Path -LiteralPath "$tempOut\$name")) { continue }
        [KeyedUpscale]::Finish("$tempOut\$name", "$Assets\x2\$($dir.Name)\$name", $info[$name].Mask, $info[$name].W, $info[$name].H)
        $done++
        if ($null -ne $info[$name].Mask) { $keyed++ }
    }
    $total += $done
    "$($dir.Name): $done of $($info.Count)"
}
Remove-Item -Recurse -Force $tempIn, $tempOut -ErrorAction SilentlyContinue
"upscaled $total pictures ($keyed with transparency) -> $Assets\x2"
