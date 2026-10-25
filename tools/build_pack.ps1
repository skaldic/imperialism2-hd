# Packs the HD art into one file of ready-to-use pixels, assets\x2\hd.pack, so the game reads a
# picture with one small read instead of opening and decoding a PNG (about 30 times faster), and
# can warm the whole pack with one sequential read at startup.
#
# Holds every map tile (x2\_tiles) and every picture of an archive up to 512x512 at 2x (units,
# towns, icons); larger pictures (full screens) stay PNG only. Run again after the art changes;
# upscale_all.ps1 and grain_tiles.ps1 do. Pictures missing from the pack are read as PNG.
#
# Layout (little-endian): "I2HP", version 1, count; then count entries of
#   key (FNV-1a of the lower-case path below x2 without ".png", e.g. "maptiles\400"),
#   width, height (16 bits each, 2x pixels), offset of the pixels (32 bits);
# sorted by key; then the pixels, 3 bytes per pixel (blue, green, red), rows top-down.
param([string]$Assets = (Join-Path $PSScriptRoot "..\assets"), [int]$MaxSide = 512)
$ErrorActionPreference = "Stop"
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

public static class HdPack
{
    public static uint Key(string relative)
    {
        uint h = 2166136261u;
        foreach (char c in relative.ToLowerInvariant()) { h ^= (byte)c; h *= 16777619u; }
        return h;
    }

    public static string Build(string x2, string[] files, int maxSide)
    {
        var entries = new List<Tuple<uint, int, int, byte[]>>();
        var seen = new HashSet<uint>();
        foreach (string file in files)
        {
            string rel = file.Substring(x2.Length).TrimStart('\\');
            rel = rel.Substring(0, rel.Length - 4); // without ".png"
            uint key = Key(rel);
            if (!seen.Add(key)) continue;
            using (var bmp = new Bitmap(file))
            {
                if (bmp.Width > maxSide || bmp.Height > maxSide) continue;
                var data = bmp.LockBits(new Rectangle(0, 0, bmp.Width, bmp.Height), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
                var px = new byte[bmp.Width * bmp.Height * 3];
                for (int y = 0; y < bmp.Height; y++)
                    Marshal.Copy(data.Scan0 + y * data.Stride, px, y * bmp.Width * 3, bmp.Width * 3);
                bmp.UnlockBits(data);
                entries.Add(Tuple.Create(key, bmp.Width, bmp.Height, px));
            }
        }
        entries.Sort((a, b) => a.Item1.CompareTo(b.Item1));
        string path = Path.Combine(x2, "hd.pack"), temp = path + ".new";
        long total = 0;
        using (var w = new BinaryWriter(File.Create(temp)))
        {
            w.Write(new[] { (byte)'I', (byte)'2', (byte)'H', (byte)'P' });
            w.Write(1); w.Write(entries.Count);
            long offset = 12 + 12L * entries.Count;
            foreach (var e in entries)
            {
                w.Write(e.Item1); w.Write((ushort)e.Item2); w.Write((ushort)e.Item3); w.Write((uint)offset);
                offset += e.Item4.Length;
            }
            foreach (var e in entries) { w.Write(e.Item4); total += e.Item4.Length; }
        }
        if (File.Exists(path)) File.Delete(path);
        File.Move(temp, path);
        return string.Format("{0} pictures, {1:N0} MB -> {2}", entries.Count, total / 1048576.0, path);
    }
}
'@
$x2 = (Resolve-Path (Join-Path $Assets "x2")).Path
$files = Get-ChildItem -LiteralPath $x2 -Directory | Where-Object { $_.Name -notlike '*_wide*' } |
    ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -Filter *.png -File } | ForEach-Object { $_.FullName }
[HdPack]::Build($x2, [string[]]$files, $MaxSide)
