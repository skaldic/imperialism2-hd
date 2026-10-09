# Upscales the extracted pictures 2x: Real-ESRGAN x4plus at 4x, reduced to 2x.
#   assets\orig\<archive>\<id>.png  ->  assets\x2\<archive>\<id>.png
#
# Terrain tiles repeat edge to edge, so each is upscaled inside a 3x3 block of itself and the
# middle cut out, which keeps the joins seamless. The upscaler also smooths their grain away, so
# part of a plain bicubic enlargement is blended back in.
#
# Sprites mark transparent pixels with a key colour (magenta). Left in, the upscaler would smear
# it into the sprite's edges, so before upscaling the key area is filled with colours bled out
# from the sprite, and afterwards the key is stamped back from a pixel-doubled copy of the mask.
param(
    [string]$Assets = "$PSScriptRoot\..\assets",
    [string]$Upscaler = "D:\Tools\realesrgan\realesrgan-ncnn-vulkan.exe",
    [string[]]$Only = @(),              # archive names to process; empty means all
    [string[]]$Terrain = @('MapTiles'), # archives whose key-less 64x64 pictures are repeating terrain
    [double]$Grain = 0.75,              # share of a plain enlargement used away from outlines (0 = none)
    [double]$EdgeLow = 8,               # blurred-brightness step below which nothing counts as an outline
    [double]$EdgeHigh = 20,            # ... and at which it fully does
    [switch]$NewOnly                    # skip pictures whose upscaled version is already up to date
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

    static int[] Resample(Bitmap source, int w, int h)
    {
        using (var scaled = new Bitmap(w, h, PixelFormat.Format32bppArgb))
        {
            using (var g = Graphics.FromImage(scaled))
            using (var attrs = new ImageAttributes())
            {
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.PixelOffsetMode = PixelOffsetMode.Half;
                attrs.SetWrapMode(WrapMode.TileFlipXY); // no dark fringe at the picture's edges
                g.DrawImage(source, new Rectangle(0, 0, w, h), 0, 0, source.Width, source.Height, GraphicsUnit.Pixel, attrs);
            }
            return Read(scaled);
        }
    }

    static float[] Box3(float[] v, int w, int h)
    {
        float[] o = new float[v.Length];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                float sum = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        sum += v[Math.Min(h - 1, Math.Max(0, y + dy)) * w + Math.Min(w - 1, Math.Max(0, x + dx))];
                o[y * w + x] = sum / 9;
            }
        return o;
    }

    // 0..1 per pixel: how strongly a real outline passes here. The picture is blurred first, so
    // dither and grain (which change from pixel to pixel) cancel out and drawn lines remain.
    static float[] EdgeWeight(int[] px, int w, int h)
    {
        float[] lum = new float[px.Length];
        for (int i = 0; i < px.Length; i++)
            lum[i] = (((px[i] >> 16) & 255) * 2 + ((px[i] >> 8) & 255) * 5 + (px[i] & 255)) / 8f;
        lum = Box3(Box3(lum, w, h), w, h);
        float[] edge = new float[px.Length];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                int x0 = Math.Max(0, x - 1), x1 = Math.Min(w - 1, x + 1), y0 = Math.Max(0, y - 1), y1 = Math.Min(h - 1, y + 1);
                float gx = lum[y * w + x1] - lum[y * w + x0], gy = lum[y1 * w + x] - lum[y0 * w + x];
                float mag = (float)Math.Sqrt(gx * gx + gy * gy);
                edge[y * w + x] = Math.Min(1f, Math.Max(0f, (mag - EdgeLow) / (EdgeHigh - EdgeLow)));
            }
        // Spread each outline a little so the sharp result is used on both of its sides.
        float[] wide = new float[px.Length];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                float m = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        m = Math.Max(m, edge[Math.Min(h - 1, Math.Max(0, y + dy)) * w + Math.Min(w - 1, Math.Max(0, x + dx))]);
                wide[y * w + x] = m;
            }
        return Box3(wide, w, h);
    }

    public static float EdgeLow = 3f, EdgeHigh = 10f;

    // Doubles a transparency mask with the Scale2x rule, which rounds diagonal steps instead of
    // leaving them as 2x2 blocks. Checkerboards (stippled shadows) are left alone by the rule.
    static bool[] DoubleMask(bool[] mask, int w, int h)
    {
        bool[] o = new bool[w * h * 4];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                bool p = mask[y * w + x];
                bool u = y == 0 || mask[(y - 1) * w + x], d = y == h - 1 || mask[(y + 1) * w + x];
                bool l = x == 0 || mask[y * w + x - 1], r = x == w - 1 || mask[y * w + x + 1];
                bool tl = p, tr = p, bl = p, br = p;
                if (u != d && l != r)
                {
                    if (l == u) tl = l;
                    if (u == r) tr = r;
                    if (l == d) bl = l;
                    if (d == r) br = r;
                }
                int at = (y * 2) * (w * 2) + x * 2;
                o[at] = tl; o[at + 1] = tr; o[at + w * 2] = bl; o[at + w * 2 + 1] = br;
            }
        return o;
    }

    // Combines the upscaler's result with a plain enlargement and stamps the key colour back.
    // "prepared" is the picture as it was given to the upscaler. Along real outlines the
    // upscaler's result is used as it is; elsewhere "grain" of it is replaced by the plain
    // enlargement, which keeps dithered textures from being redrawn as patterns the artist never
    // made.
    public static void Finish(string upscaled, string prepared, string dest, bool[] mask, int width, int height, double grain)
    {
        int w2 = width * 2, h2 = height * 2;
        int[] model, plain, small;
        using (var big = new Bitmap(upscaled))
        using (var sm = new Bitmap(prepared))
        {
            model = Resample(big, w2, h2);
            plain = Resample(sm, w2, h2);
            small = Read(sm);
        }
        float[] edge = EdgeWeight(small, width, height);
        bool[] mask2 = mask == null ? null : DoubleMask(mask, width, height);
        int[] px = new int[w2 * h2];
        for (int y = 0; y < h2; y++)
            for (int x = 0; x < w2; x++)
            {
                int i = y * w2 + x;
                if (mask2 != null && mask2[i]) { px[i] = unchecked((int)0xffff00ff); continue; }
                // The edge weight is at 1x; take the nearer of the two samples on each axis.
                float e = edge[(y / 2) * width + x / 2];
                float share = (float)((1 - grain) + grain * e); // of the upscaler's result
                int c = unchecked((int)0xff000000);
                for (int shift = 0; shift <= 16; shift += 8)
                {
                    float v = ((model[i] >> shift) & 255) * share + ((plain[i] >> shift) & 255) * (1 - share);
                    c |= Math.Min(255, Math.Max(0, (int)(v + 0.5f))) << shift;
                }
                px[i] = c;
            }
        using (var result = Write(px, w2, h2)) Save24(result, dest);
    }

    // Writes a 3x3 block of the tile for the upscaler.
    public static void PrepareTile(string source, string dest)
    {
        using (var src = new Bitmap(source))
        using (var block = new Bitmap(src.Width * 3, src.Height * 3, PixelFormat.Format24bppRgb))
        {
            using (var g = Graphics.FromImage(block))
                for (int y = 0; y < 3; y++)
                    for (int x = 0; x < 3; x++)
                        g.DrawImage(src, x * src.Width, y * src.Height, src.Width, src.Height);
            block.Save(dest, ImageFormat.Png);
        }
    }

    // Cuts the middle tile out of the upscaled 3x3 block at 2x and blends in the original's grain.
    public static void FinishTile(string upscaled, string original, string dest, double grain)
    {
        using (var big = new Bitmap(upscaled))
        using (var src = new Bitmap(original))
        using (var outBmp = new Bitmap(src.Width * 2, src.Height * 2, PixelFormat.Format32bppArgb))
        {
            var all = new Rectangle(0, 0, outBmp.Width, outBmp.Height);
            using (var g = Graphics.FromImage(outBmp))
            {
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.PixelOffsetMode = PixelOffsetMode.Half;
                g.DrawImage(big, all, big.Width / 3, big.Height / 3, big.Width / 3, big.Height / 3, GraphicsUnit.Pixel);
                using (var attrs = new ImageAttributes())
                {
                    attrs.SetWrapMode(WrapMode.Tile); // the tile's own far edge is its neighbour
                    var fade = new ColorMatrix();
                    fade.Matrix33 = (float)grain;
                    attrs.SetColorMatrix(fade);
                    g.DrawImage(src, all, 0, 0, src.Width, src.Height, GraphicsUnit.Pixel, attrs);
                }
            }
            Save24(outBmp, dest);
        }
    }

    static void Save24(Bitmap bmp, string dest)
    {
        using (var rgb = bmp.Clone(new Rectangle(0, 0, bmp.Width, bmp.Height), PixelFormat.Format24bppRgb))
            rgb.Save(dest, ImageFormat.Png);
    }
}
'@

[KeyedUpscale]::EdgeLow = $EdgeLow
[KeyedUpscale]::EdgeHigh = $EdgeHigh
$orig = "$Assets\orig"
$tempIn = "$Assets\_in$PID"   # per process, so a run started by the game cannot collide with another
$tempOut = "$Assets\_out$PID"
$total = 0; $keyed = 0; $tiles = 0
foreach ($dir in Get-ChildItem -LiteralPath $orig -Directory) {
    if ($Only.Count -gt 0 -and $Only -notcontains $dir.Name) { continue }
    Remove-Item -Recurse -Force $tempIn, $tempOut -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $tempIn, $tempOut, "$Assets\x2\$($dir.Name)" | Out-Null

    $info = @{}
    foreach ($file in Get-ChildItem -LiteralPath $dir.FullName -Filter *.png) {
        if ($NewOnly) {
            $have = Get-Item -LiteralPath "$Assets\x2\$($dir.Name)\$($file.Name)" -ErrorAction SilentlyContinue
            if ($have -and $have.LastWriteTime -ge $file.LastWriteTime) { continue }
        }
        $w = 0; $h = 0
        $mask = [KeyedUpscale]::Prepare($file.FullName, "$tempIn\$($file.Name)", [ref]$w, [ref]$h)
        $tile = $null -eq $mask -and $Terrain -contains $dir.Name -and $w -eq 64 -and $h -eq 64
        if ($tile) { [KeyedUpscale]::PrepareTile($file.FullName, "$tempIn\$($file.Name)") }
        $info[$file.Name] = @{ Mask = $mask; W = $w; H = $h; Tile = $tile; Source = $file.FullName }
    }
    if ($info.Count -eq 0) { continue }
    & $Upscaler -i $tempIn -o $tempOut -n realesrgan-x4plus -s 4 -f png 2>$null

    $done = 0
    foreach ($name in $info.Keys) {
        if (-not (Test-Path -LiteralPath "$tempOut\$name")) { continue }
        $dest = "$Assets\x2\$($dir.Name)\$name"
        if ($info[$name].Tile) { [KeyedUpscale]::FinishTile("$tempOut\$name", $info[$name].Source, $dest, $Grain); $tiles++ }
        else { [KeyedUpscale]::Finish("$tempOut\$name", "$tempIn\$name", $dest, $info[$name].Mask, $info[$name].W, $info[$name].H, $Grain) }
        $done++
        if ($null -ne $info[$name].Mask) { $keyed++ }
    }
    $total += $done
    "$($dir.Name): $done of $($info.Count)"
}
Remove-Item -Recurse -Force $tempIn, $tempOut -ErrorAction SilentlyContinue
"upscaled $total pictures ($keyed with transparency, $tiles terrain tiles) -> $Assets\x2"
