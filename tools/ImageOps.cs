using System.Collections.Generic;
// Pixel helpers for the tools scripts (PowerShell's GetPixel is far too slow at 4x).
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class ImageOps
{
    public static int[] Read(Bitmap b)
    {
        var d = b.LockBits(new Rectangle(0, 0, b.Width, b.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppRgb);
        var px = new int[b.Width * b.Height];
        for (int y = 0; y < b.Height; y++) Marshal.Copy(d.Scan0 + y * d.Stride, px, y * b.Width, b.Width);
        b.UnlockBits(d);
        return px;
    }

    public static Bitmap Write(int[] px, int w, int h)
    {
        var b = new Bitmap(w, h, PixelFormat.Format32bppRgb);
        var d = b.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppRgb);
        for (int y = 0; y < h; y++) Marshal.Copy(px, y * w, d.Scan0 + y * d.Stride, w);
        b.UnlockBits(d);
        return b;
    }

    static void ToLab(int p, out double L, out double A, out double B)
    {
        double r = Lin(((p >> 16) & 255) / 255.0), g = Lin(((p >> 8) & 255) / 255.0), b = Lin((p & 255) / 255.0);
        double l = Math.Pow(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b, 1 / 3.0);
        double m = Math.Pow(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b, 1 / 3.0);
        double s = Math.Pow(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b, 1 / 3.0);
        L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
        A = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
        B = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    }

    static int FromLab(double L, double A, double B)
    {
        double l = L + 0.3963377774 * A + 0.2158037573 * B, m = L - 0.1055613458 * A - 0.0638541728 * B, s = L - 0.0894841775 * A - 1.2914855480 * B;
        l = l * l * l; m = m * m * m; s = s * s * s;
        double r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
        double g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
        double b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
        return (Enc(r) << 16) | (Enc(g) << 8) | Enc(b);
    }

    static double Lin(double c) { return c <= 0.04045 ? c / 12.92 : Math.Pow((c + 0.055) / 1.055, 2.4); }
    static int Enc(double c)
    {
        c = c <= 0 ? 0 : c >= 1 ? 1 : c;
        c = c <= 0.0031308 ? c * 12.92 : 1.055 * Math.Pow(c, 1 / 2.4) - 0.055;
        return (int)Math.Round(c * 255);
    }

    // Teal-green pixels (the study's wallpaper) in a rectangle: count and mean Lab.
    public static double[] TealStats(int[] px, int w, int x0, int y0, int x1, int y1)
    {
        double n = 0, sl = 0, sa = 0, sb = 0, sll = 0;
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++)
            {
                double L, A, B; ToLab(px[y * w + x], out L, out A, out B);
                if (!IsTeal(L, A, B)) continue;
                n++; sl += L; sa += A; sb += B; sll += L * L;
            }
        if (n == 0) return new double[] { 0, 0, 0, 0, 0 };
        return new double[] { n, sl / n, sa / n, sb / n, Math.Sqrt(Math.Max(0, sll / n - (sl / n) * (sl / n))) };
    }

    static bool IsTeal(double L, double A, double B)
    {
        double c = Math.Sqrt(A * A + B * B), h = Math.Atan2(B, A) * 180 / Math.PI;
        return c > 0.025 && L > 0.12 && L < 0.6 && h > 150 && h < 260;
    }

    // Moves teal pixels of columns [x0, x1) towards a target mean (and spread) of lightness and
    // colour, weighted by how teal they are and by `weight` (0..1) per column.
    public static void MatchTeal(int[] px, int w, int h, int x0, int x1, double[] from, double[] to, double[] weight)
    {
        double kL = from[4] > 1e-4 ? to[4] / from[4] : 1;
        for (int y = 0; y < h; y++)
            for (int x = x0; x < x1; x++)
            {
                double wt = weight[x - x0];
                if (wt <= 0) continue;
                int p = px[y * w + x];
                double L, A, B; ToLab(p, out L, out A, out B);
                double c = Math.Sqrt(A * A + B * B), hue = Math.Atan2(B, A) * 180 / Math.PI;
                // Soft membership: full inside the teal range, fading out over 25 degrees.
                double mh = hue >= 150 && hue <= 260 ? 1 : Math.Max(0, 1 - Math.Min(Math.Abs(hue - 150), Math.Abs(hue - 260)) / 25);
                double mc = Math.Min(1, Math.Max(0, (c - 0.012) / 0.02));
                double ml = L < 0.08 || L > 0.7 ? 0 : 1;
                double t = wt * mh * mc * ml;
                if (t <= 0) continue;
                double nl = (L - from[1]) * kL + to[1], na = A - from[2] + to[2], nb = B - from[3] + to[3];
                px[y * w + x] = FromLab(L + (nl - L) * t, A + (na - A) * t, B + (nb - B) * t);
            }
    }

    public static double Lum(int p) { double L, A, B; ToLab(p, out L, out A, out B); return L; }

    // Repeat of a texture in a rectangle: the shift (dx, dy) with dx in [minP, maxP] or dy too,
    // that best matches the texture with itself. Returns dx, dy, score.
    public static double[] Period(int[] px, int w, int x0, int y0, int x1, int y1, int maxP)
    {
        int rw = x1 - x0, rh = y1 - y0;
        var L = new double[rw * rh];
        for (int y = 0; y < rh; y++) for (int x = 0; x < rw; x++) L[y * rw + x] = Lum(px[(y0 + y) * w + x0 + x]);
        double best = 1e9; int bx = 0, by = 0;
        for (int dy = 0; dy <= maxP; dy++)
            for (int dx = -maxP; dx <= maxP; dx++)
            {
                if (dx * dx + dy * dy < 36 || (dy == 0 && dx < 0)) continue;
                double sum = 0; int n = 0;
                for (int y = 0; y + dy < rh; y++)
                    for (int x = Math.Max(0, -dx); x < rw && x + dx < rw; x++) { sum += Math.Abs(L[y * rw + x] - L[(y + dy) * rw + x + dx]); n++; }
                if (n < rw * rh / 4) continue;
                double d = sum / n;
                if (d < best) { best = d; bx = dx; by = dy; }
            }
        return new double[] { bx, by, best };
    }

    // Lab planes of a rectangle, with a box blur of radius r (clamped at the edges).
    static double[][] Planes(int[] px, int w, int x0, int y0, int x1, int y1)
    {
        int rw = x1 - x0, rh = y1 - y0;
        var P = new double[3][]; for (int c = 0; c < 3; c++) P[c] = new double[rw * rh];
        for (int y = 0; y < rh; y++)
            for (int x = 0; x < rw; x++)
            {
                double L, A, B; ToLab(px[(y0 + y) * w + x0 + x], out L, out A, out B);
                P[0][y * rw + x] = L; P[1][y * rw + x] = A; P[2][y * rw + x] = B;
            }
        return P;
    }

    static double[] Blur(double[] v, int rw, int rh, int r)
    {
        var t = new double[v.Length]; var o = new double[v.Length];
        for (int y = 0; y < rh; y++)
        {
            double sum = 0; int n = 0;
            for (int x = -r; x <= r; x++) { int xx = Math.Min(rw - 1, Math.Max(0, x)); sum += v[y * rw + xx]; n++; }
            for (int x = 0; x < rw; x++)
            {
                t[y * rw + x] = sum / n;
                int add = Math.Min(rw - 1, x + r + 1), rem = Math.Max(0, x - r);
                sum += v[y * rw + add] - v[y * rw + rem];
            }
        }
        for (int x = 0; x < rw; x++)
        {
            double sum = 0; int n = 0;
            for (int y = -r; y <= r; y++) { int yy = Math.Min(rh - 1, Math.Max(0, y)); sum += t[yy * rw + x]; n++; }
            for (int y = 0; y < rh; y++)
            {
                o[y * rw + x] = sum / n;
                int add = Math.Min(rh - 1, y + r + 1), rem = Math.Max(0, y - r);
                sum += t[add * rw + x] - t[rem * rw + x];
            }
        }
        return o;
    }

    static double TealWeight(double L, double A, double B)
    {
        double c = Math.Sqrt(A * A + B * B), hue = Math.Atan2(B, A) * 180 / Math.PI;
        double mh = hue >= 150 && hue <= 260 ? 1 : Math.Max(0, 1 - Math.Min(Math.Abs(hue - 150), Math.Abs(hue - 260)) / 20);
        double mc = Math.Min(1, Math.Max(0, (c - 0.012) / 0.015));
        double ml = L < 0.06 || L > 0.55 ? 0 : 1;
        return mh * mc * ml;
    }

    static double GoldWeight(double L, double A, double B)
    {
        double c = Math.Sqrt(A * A + B * B), hue = Math.Atan2(B, A) * 180 / Math.PI;
        double mh = hue >= 60 && hue <= 105 ? 1 : Math.Max(0, 1 - Math.Min(Math.Abs(hue - 60), Math.Abs(hue - 105)) / 15);
        double mc = Math.Min(1, Math.Max(0, (c - 0.04) / 0.03));
        return mh * mc * (L > 0.2 ? 1 : 0);
    }

    // 0 at ramp0 rising to 1 at ramp1 (either direction).
    static double Ramp(int x, int ramp0, int ramp1) { double t = (x - ramp0 + 0.5) / (double)(ramp1 - ramp0); return t < 0 ? 0 : t > 1 ? 1 : t; }

    static int Mirror(int v, int n) { int p = 2 * n; v %= p; if (v < 0) v += p; return v < n ? v : p - 1 - v; }

    // Wallpaper: in columns [x0, x1) of the target, teal pixels keep their own shading (low-pass)
    // and colour shifted to the source's, and get the source's fine pattern (its high-pass,
    // mirror-tiled from the source rectangle).
    public static string ReplaceWallpaper(int[] px, int w, int h, int x0, int x1, int sx0, int sy0, int sx1, int sy1, int r, int ramp0, int ramp1)
    {
        var S = Planes(px, w, sx0, sy0, sx1, sy1);
        int sw = sx1 - sx0, sh = sy1 - sy0;
        var Sl = new double[3][]; var Sd = new double[3][]; var Smean = new double[3];
        for (int c = 0; c < 3; c++)
        {
            Sl[c] = Blur(S[c], sw, sh, r); Sd[c] = new double[S[c].Length];
            for (int i = 0; i < S[c].Length; i++) { Sd[c][i] = S[c][i] - Sl[c][i]; Smean[c] += S[c][i]; }
            Smean[c] /= S[c].Length;
        }
        var T = Planes(px, w, x0, 0, x1, h);
        int tw = x1 - x0;
        var Tl = new double[3][]; for (int c = 0; c < 3; c++) Tl[c] = Blur(T[c], tw, h, r);
        var wt = new double[tw * h]; double n = 0; var Tmean = new double[3];
        for (int i = 0; i < wt.Length; i++)
        {
            wt[i] = TealWeight(T[0][i], T[1][i], T[2][i]);
            if (wt[i] > 0.5) { n++; for (int c = 0; c < 3; c++) Tmean[c] += Tl[c][i]; }
        }
        if (n == 0) return "no wallpaper";
        for (int c = 0; c < 3; c++) Tmean[c] /= n;
        // The weight is smoothed so the pattern fades in and out instead of switching per pixel.
        wt = Blur(wt, tw, h, Math.Max(1, r / 3));
        for (int y = 0; y < h; y++)
            for (int x = 0; x < tw; x++)
            {
                int i = y * tw + x; double t = Math.Min(1, wt[i] * 1.4 - 0.2) * Ramp(x0 + x, ramp0, ramp1);
                if (t <= 0) continue;
                int si = Mirror(y, sh) * sw + Mirror(x, sw);
                var v = new double[3];
                for (int c = 0; c < 3; c++) v[c] = Tl[c][i] + (Smean[c] - Tmean[c]) + Sd[c][si];
                int p0 = FromLab(v[0], v[1], v[2]);
                double L = T[0][i] + (v[0] - T[0][i]) * t, A = T[1][i] + (v[1] - T[1][i]) * t, B = T[2][i] + (v[2] - T[2][i]) * t;
                px[y * w + x0 + x] = FromLab(L, A, B);
            }
        return string.Format("wallpaper: {0} px, shift L {1:N3} a {2:N3} b {3:N3}", n, Smean[0] - Tmean[0], Smean[1] - Tmean[1], Smean[2] - Tmean[2]);
    }

    // Spread of fine detail (pixel minus its blur) and mean of gold pixels in a rectangle.
    public static double[] GoldStats(int[] px, int w, int x0, int y0, int x1, int y1, int r)
    {
        var P = Planes(px, w, x0, y0, x1, y1); int rw = x1 - x0, rh = y1 - y0;
        var Lb = Blur(P[0], rw, rh, r);
        double n = 0, s2 = 0, sl = 0, sc = 0;
        for (int i = 0; i < P[0].Length; i++)
        {
            if (GoldWeight(P[0][i], P[1][i], P[2][i]) < 0.5) continue;
            double d = P[0][i] - Lb[i]; n++; s2 += d * d; sl += P[0][i]; sc += Math.Sqrt(P[1][i] * P[1][i] + P[2][i] * P[2][i]);
        }
        return n == 0 ? new double[] { 0, 0, 0, 0 } : new double[] { n, Math.Sqrt(s2 / n), sl / n, sc / n };
    }

    // Gold: fine detail amplified by `gain`, lightness and colourfulness moved to the target's.
    public static void SharpenGold(int[] px, int w, int h, int x0, int x1, int r, double gain, double dL, double chromaK, int ramp0, int ramp1)
    {
        var T = Planes(px, w, x0, 0, x1, h); int tw = x1 - x0;
        var Lb = Blur(T[0], tw, h, r);
        var wt = new double[tw * h];
        for (int i = 0; i < wt.Length; i++) wt[i] = GoldWeight(T[0][i], T[1][i], T[2][i]);
        wt = Blur(wt, tw, h, 1);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < tw; x++)
            {
                int i = y * tw + x; double t = wt[i] * Ramp(x0 + x, ramp0, ramp1); if (t <= 0.02) continue;
                double L = Lb[i] + (T[0][i] - Lb[i]) * gain + dL, A = T[1][i] * chromaK, B = T[2][i] * chromaK;
                px[y * w + x0 + x] = FromLab(T[0][i] + (L - T[0][i]) * t, T[1][i] + (A - T[1][i]) * t, T[2][i] + (B - T[2][i]) * t);
            }
    }

    // Unsharp mask on the sprite's own pixels: each colour channel moved away from a small blur by
    // `amount`. Pixels of the key colour (magenta, transparent) are left alone and do not count in
    // the blur, so the sprite's edge does not pick up a pink fringe.
    public static void SharpenSprite(int[] px, int w, int h, double amount, int key)
    {
        var src = (int[])px.Clone();
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                int i = y * w + x, p = src[i];
                if ((p & 0xffffff) == key) continue;
                double r = 0, g = 0, b = 0, n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                    {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                        int q = src[yy * w + xx];
                        if ((q & 0xffffff) == key) continue;
                        double wt = dx == 0 && dy == 0 ? 4 : dx == 0 || dy == 0 ? 2 : 1;
                        r += ((q >> 16) & 255) * wt; g += ((q >> 8) & 255) * wt; b += (q & 255) * wt; n += wt;
                    }
                r /= n; g /= n; b /= n;
                int nr = Clamp((int)Math.Round(((p >> 16) & 255) + amount * (((p >> 16) & 255) - r)));
                int ng = Clamp((int)Math.Round(((p >> 8) & 255) + amount * (((p >> 8) & 255) - g)));
                int nb = Clamp((int)Math.Round((p & 255) + amount * ((p & 255) - b)));
                if ((nr << 16 | ng << 8 | nb) == key) nb--; // never turn a pixel into the key
                px[i] = (int)(0xff000000u | (uint)(nr << 16 | ng << 8 | nb));
            }
    }

    static int Clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

    // Puts a tile's own fine grain back into its 2x version: each 2x pixel gets `amount` times the
    // difference between the 1x pixel under it and that pixel's 3x3 average (the tile wraps at its
    // edges, as terrain does). Shading and colour stay the upscale's; only the speckle returns.
    public static void AddGrain(int[] hd, int w2, int h2, int[] orig, int w, int h, double amount)
    {
        var detail = new double[w * h * 3];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                double r = 0, g = 0, b = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                    {
                        int q = orig[((y + dy + h) % h) * w + (x + dx + w) % w];
                        r += (q >> 16) & 255; g += (q >> 8) & 255; b += q & 255;
                    }
                int p = orig[y * w + x], i = (y * w + x) * 3;
                detail[i] = ((p >> 16) & 255) - r / 9; detail[i + 1] = ((p >> 8) & 255) - g / 9; detail[i + 2] = (p & 255) - b / 9;
            }
        for (int y = 0; y < h2; y++)
            for (int x = 0; x < w2; x++)
            {
                int i = ((y * h / h2) * w + x * w / w2) * 3, p = hd[y * w2 + x];
                int nr = Clamp((int)Math.Round(((p >> 16) & 255) + amount * detail[i]));
                int ng = Clamp((int)Math.Round(((p >> 8) & 255) + amount * detail[i + 1]));
                int nb = Clamp((int)Math.Round((p & 255) + amount * detail[i + 2]));
                hd[y * w2 + x] = (int)(0xff000000u | (uint)(nr << 16 | ng << 8 | nb));
            }
    }

    // Lays a generated subject onto the original tile: generated pixels that are not grass (not a
    // clearly green hue) are the subject; the mask is grown by one pixel and softened, and the
    // rest of the tile stays the original grass.
    public static void PasteSubject(int[] orig, int[] gen, int w, int h, double cx, double cy, double radius)
    {
        var m = new double[w * h];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                double dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy > radius * radius) continue;
                int p = gen[y * w + x], r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
                bool grass = g > r + 12 && g > b + 12;
                if (!grass) m[y * w + x] = 1;
            }
        var grown = new double[w * h];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                double v = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                    {
                        int xx = x + dx, yy = y + dy;
                        if (xx >= 0 && yy >= 0 && xx < w && yy < h && m[yy * w + xx] > v) v = m[yy * w + xx];
                    }
                grown[y * w + x] = v;
            }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                double a = 0; int n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                    {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                        a += grown[yy * w + xx]; n++;
                    }
                a /= n;
                int i = y * w + x, o = orig[i], g2 = gen[i], v = 0;
                for (int sh = 0; sh < 24; sh += 8)
                    v |= (int)(((o >> sh) & 255) * (1 - a) + ((g2 >> sh) & 255) * a) << sh;
                orig[i] = (int)(0xff000000u | (uint)v);
            }
    }

    // Uneven grass: smooth random patches, wrapping at the tile's edges so the tile still repeats
    // seamlessly. Light patches turn slightly yellow (dry), dark ones deeper green (clumps).
    // `amount` is the brightness change at the strongest patch (0.15 = 15%).
    public static void Mottle(int[] px, int w, int h, double amount, int seed)
    {
        var field = new double[w * h];
        var rnd = new Random(seed);
        foreach (int cells in new[] { 3, 6, 12 })
        {
            var lat = new double[cells * cells];
            for (int i = 0; i < lat.Length; i++) lat[i] = rnd.NextDouble() * 2 - 1;
            double weight = cells == 3 ? 0.5 : cells == 6 ? 0.35 : 0.15;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                {
                    double fx = (double)x * cells / w, fy = (double)y * cells / h;
                    int x0 = (int)fx, y0 = (int)fy; double tx = fx - x0, ty = fy - y0;
                    tx = tx * tx * (3 - 2 * tx); ty = ty * ty * (3 - 2 * ty);
                    int x1 = (x0 + 1) % cells, y1 = (y0 + 1) % cells;
                    double a = lat[y0 * cells + x0] * (1 - tx) + lat[y0 * cells + x1] * tx;
                    double b = lat[y1 * cells + x0] * (1 - tx) + lat[y1 * cells + x1] * tx;
                    field[y * w + x] += weight * (a * (1 - ty) + b * ty);
                }
        }
        double max = 1e-9; foreach (double v in field) max = Math.Max(max, Math.Abs(v));
        for (int i = 0; i < w * h; i++)
        {
            double f = field[i] / max, k = 1 + amount * f;
            int p = px[i], r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            double dry = Math.Max(0, f) * amount * 0.6; // light patches drift toward straw
            int nr = Clamp((int)Math.Round(r * k + dry * 90));
            int ng = Clamp((int)Math.Round(g * k + dry * 40));
            int nb = Clamp((int)Math.Round(b * k));
            px[i] = (int)(0xff000000u | (uint)(nr << 16 | ng << 8 | nb));
        }
    }

    // The painting's own grass: the median colour of a ring just inside the tile's edge.
    public static int GrassColour(int[] px, int w, int h)
    {
        var rs = new List<int>(); var gs = new List<int>(); var bs = new List<int>();
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                int e = Math.Min(Math.Min(x, y), Math.Min(w - 1 - x, h - 1 - y));
                if (e < w / 32 || e > w / 12) continue;
                int p = px[y * w + x]; rs.Add((p >> 16) & 255); gs.Add((p >> 8) & 255); bs.Add(p & 255);
            }
        rs.Sort(); gs.Sort(); bs.Sort(); int m = rs.Count / 2;
        return rs[m] << 16 | gs[m] << 8 | bs[m];
    }

    // True where a pixel is far enough from the grass colour to be part of the subject.
    public static bool[] SubjectMask(int[] px, int w, int h, int grass, int tolerance)
    {
        int gr = (grass >> 16) & 255, gg = (grass >> 8) & 255, gb = grass & 255;
        var m = new bool[w * h];
        for (int i = 0; i < w * h; i++)
        {
            int p = px[i], dr = ((p >> 16) & 255) - gr, dg = ((p >> 8) & 255) - gg, db = (p & 255) - gb;
            m[i] = Math.Sqrt(dr * dr + dg * dg + db * db) > tolerance;
        }
        return m;
    }

    // The square around the subject: rows and columns with at least `minRun` subject pixels
    // (stray specks don't count), grown by `margin` of its size. Returns x, y, size.
    public static int[] SubjectSquare(bool[] m, int w, int h, int minRun, double margin)
    {
        int x0 = w, x1 = -1, y0 = h, y1 = -1;
        for (int y = 0; y < h; y++) { int n = 0; for (int x = 0; x < w; x++) if (m[y * w + x]) n++; if (n >= minRun) { y0 = Math.Min(y0, y); y1 = y; } }
        for (int x = 0; x < w; x++) { int n = 0; for (int y = 0; y < h; y++) if (m[y * w + x]) n++; if (n >= minRun) { x0 = Math.Min(x0, x); x1 = x; } }
        if (x1 < 0 || y1 < 0) return new[] { 0, 0, w };
        int size = (int)(Math.Max(x1 - x0, y1 - y0) * (1 + 2 * margin));
        size = Math.Min(Math.Max(size, w / 4), w);
        int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        int sx = Math.Min(Math.Max(cx - size / 2, 0), w - size), sy = Math.Min(Math.Max(cy - size / 2, 0), h - size);
        return new[] { sx, sy, size };
    }

    // Lays the subject (mask grown by one pixel, edge softened) onto the original tile.
    public static void PasteMasked(int[] orig, int[] gen, bool[] m, int w, int h)
    {
        var a = new double[w * h];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                int n = 0, hit = 0;
                for (int dy = -2; dy <= 2; dy++)
                    for (int dx = -2; dx <= 2; dx++)
                    {
                        int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                        n++;
                        bool near = false;
                        for (int ey = -1; ey <= 1 && !near; ey++)
                            for (int ex = -1; ex <= 1 && !near; ex++)
                            {
                                int qx = xx + ex, qy = yy + ey;
                                if (qx >= 0 && qy >= 0 && qx < w && qy < h && m[qy * w + qx]) near = true;
                            }
                        if (near) hit++;
                    }
                a[y * w + x] = (double)hit / n;
            }
        for (int i = 0; i < w * h; i++)
        {
            int o = orig[i], g = gen[i], v = 0;
            for (int sh = 0; sh < 24; sh += 8)
                v |= (int)(((o >> sh) & 255) * (1 - a[i]) + ((g >> sh) & 255) * a[i]) << sh;
            orig[i] = (int)(0xff000000u | (uint)v);
        }
    }

    // Drops thin parts of a mask (lines narrower than 2*r+1 pixels): erode, then grow back.
    public static bool[] OpenMask(bool[] m, int w, int h, int r)
    {
        Func<bool[], bool, bool[]> pass = (src, all) =>
        {
            var o = new bool[w * h];
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                {
                    bool v = all;
                    for (int dy = -r; dy <= r; dy++)
                        for (int dx = -r; dx <= r; dx++)
                        {
                            int xx = x + dx, yy = y + dy;
                            bool q = xx >= 0 && yy >= 0 && xx < w && yy < h && src[yy * w + xx];
                            if (all) v &= q; else v |= q;
                        }
                    o[y * w + x] = v;
                }
            return o;
        };
        return pass(pass(m, true), false);
    }
}