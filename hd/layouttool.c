// Offline helper for the wide screen layouts in layouts.h.
//
//   layouttool preview <layout> <in.bmp> <out.bmp> [views.txt]
//       Widens an 800-wide background the way the patch does and, given a list of the screen's
//       views ("x y w h leaf" per line, window coordinates), outlines where each one ends up:
//       containers in cyan, everything else in magenta.
//   layouttool period <in.bmp> <x0> <x1> <y0> <y1>
//       Best horizontal repeat periods of a strip of artwork, for 'T' insertions.
//
// Images are uncompressed 24- or 32-bit BMP; tools\bmp.ps1 converts to and from PNG.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "layouts.h"

typedef struct { int w, h; unsigned *px; } Image;

static int read_bmp(const char *path, Image *img)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	unsigned char hdr[54];
	if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M') { fclose(f); return 0; }
	int offset = *(int *)(hdr + 10), w = *(int *)(hdr + 18), h = *(int *)(hdr + 22), bpp = *(short *)(hdr + 28);
	int bottomUp = h > 0;
	if (h < 0) h = -h;
	if (bpp != 24 && bpp != 32) { fclose(f); return 0; }
	int stride = (w * (bpp / 8) + 3) & ~3;
	unsigned char *row = malloc(stride);
	img->w = w; img->h = h; img->px = malloc(sizeof(unsigned) * w * h);
	fseek(f, offset, SEEK_SET);
	for (int y = 0; y < h; y++) {
		if (fread(row, 1, stride, f) != (size_t)stride) break;
		unsigned *dst = img->px + (bottomUp ? h - 1 - y : y) * w;
		for (int x = 0; x < w; x++) {
			unsigned char *p = row + x * (bpp / 8);
			dst[x] = p[0] | (p[1] << 8) | (p[2] << 16);
		}
	}
	free(row);
	fclose(f);
	return 1;
}

static int write_bmp(const char *path, const Image *img)
{
	FILE *f = fopen(path, "wb");
	if (!f) return 0;
	int stride = img->w * 4;
	unsigned char hdr[54] = { 'B', 'M' };
	*(int *)(hdr + 2) = 54 + stride * img->h;
	*(int *)(hdr + 10) = 54;
	*(int *)(hdr + 14) = 40;
	*(int *)(hdr + 18) = img->w;
	*(int *)(hdr + 22) = -img->h;
	*(short *)(hdr + 26) = 1;
	*(short *)(hdr + 28) = 32;
	fwrite(hdr, 1, 54, f);
	fwrite(img->px, 4, img->w * img->h, f);
	fclose(f);
	return 1;
}

static void outline(Image *img, int x, int y, int w, int h, unsigned colour)
{
	for (int i = x; i < x + w; i++) {
		if (i < 0 || i >= img->w) continue;
		if (y >= 0 && y < img->h) img->px[y * img->w + i] = colour;
		if (y + h - 1 >= 0 && y + h - 1 < img->h) img->px[(y + h - 1) * img->w + i] = colour;
	}
	for (int j = y; j < y + h; j++) {
		if (j < 0 || j >= img->h) continue;
		if (x >= 0 && x < img->w) img->px[j * img->w + x] = colour;
		if (x + w - 1 >= 0 && x + w - 1 < img->w) img->px[j * img->w + x + w - 1] = colour;
	}
}

static int preview(const char *name, const char *in, const char *out, const char *views)
{
	const ScreenLayout *s = layout_by_name(name);
	if (!s) { fprintf(stderr, "no layout '%s'\n", name); return 1; }
	if (!layout_valid(s)) { fprintf(stderr, "layout '%s' is not valid\n", name); return 1; }
	Image src, dst;
	if (!read_bmp(in, &src) || src.w != s->width || src.h != s->height) { fprintf(stderr, "cannot read %s at %dx%d\n", in, s->width, s->height); return 1; }
	dst.w = s->width + LAYOUT_EXTRA; dst.h = s->height; dst.px = malloc(sizeof(unsigned) * dst.w * dst.h);
	short map[LAYOUT_MAX_W + 64];
	for (int b = 0; b < LAYOUT_MAX_BANDS && s->bands[b].bottom; b++) {
		const LayoutBand *band = &s->bands[b];
		layout_band_map(s, band, map);
		for (int y = band->top; y < band->bottom; y++)
			for (int x = 0; x < dst.w; x++) dst.px[y * dst.w + x] = map[x] >= 0 ? src.px[y * src.w + map[x]] : 0xff00ff;
		// Patches, mirror-tiled in both directions.
		{
			int before = 0;
			for (int i = 0; i < LAYOUT_MAX_INSERTS && band->ins[i].width; i++) {
				const LayoutInsert *in = &band->ins[i];
				if (in->mode == 'Q') {
					int start = in->at + before;
					for (int y = band->top; y < band->bottom; y++)
						for (int k = 0; k < in->width; k++)
							dst.px[y * dst.w + start + k] = src.px[(in->srcY + layout_mirror(y - band->top, in->srcH)) * src.w + in->src + layout_mirror(k, in->srcW)];
				}
				before += in->width;
			}
		}
		// Stretched columns blended between neighbours, as the patch draws them.
		int before = 0;
		for (int i = 0; i < LAYOUT_MAX_INSERTS && band->ins[i].width; i++) {
			const LayoutInsert *in = &band->ins[i];
			if (in->mode == 'S') {
				int from = in->src + before, total = in->srcW + in->width;
				for (int k = 0; k < total; k++) {
					int p = ((2 * k + 1) * in->srcW * 256) / (2 * total) - 128;
					if (p < 0) p = 0;
					int c = p >> 8, t = p & 255;
					if (c >= in->srcW - 1) { c = in->srcW - 1; t = 0; }
					for (int y = band->top; y < band->bottom; y++) {
						unsigned a = src.px[y * src.w + in->src + c], b = t ? src.px[y * src.w + in->src + c + 1] : a, v = 0;
						for (int sh = 0; sh < 24; sh += 8)
							v |= (unsigned)((((a >> sh) & 255) * (256 - t) + ((b >> sh) & 255) * t + 128) >> 8) << sh;
						dst.px[y * dst.w + from + k] = v;
					}
				}
			}
			before += in->width;
		}
	}
	if (views) {
		FILE *f = fopen(views, "r");
		int x, y, w, h, leaf;
		while (f && fscanf(f, "%d %d %d %d %d", &x, &y, &w, &h, &leaf) == 5) {
			if (x >= s->width) continue; // parked out of sight
			int nx, nw;
			layout_place(s, x, y, w, h, &nx, &nw);
			outline(&dst, nx, y, nw, h, leaf ? 0xff00ff : 0x00ffff);
		}
		if (f) fclose(f);
	}
	return write_bmp(out, &dst) ? 0 : 1;
}

static int period(const char *in, int x0, int x1, int y0, int y1)
{
	Image img;
	if (!read_bmp(in, &img)) return 1;
	double best[6] = { 1e9, 1e9, 1e9, 1e9, 1e9, 1e9 };
	int bestP[6] = { 0 };
	for (int p = 8; p < (x1 - x0) / 2; p++) {
		double sum = 0;
		int n = 0;
		for (int y = y0; y < y1; y++)
			for (int x = x0; x + p < x1; x++) {
				unsigned a = img.px[y * img.w + x], b = img.px[y * img.w + x + p];
				for (int c = 0; c < 24; c += 8) sum += abs((int)((a >> c) & 255) - (int)((b >> c) & 255));
				n++;
			}
		double d = sum / n;
		for (int i = 0; i < 6; i++) {
			if (d < best[i]) {
				memmove(best + i + 1, best + i, sizeof(double) * (5 - i));
				memmove(bestP + i + 1, bestP + i, sizeof(int) * (5 - i));
				best[i] = d; bestP[i] = p;
				break;
			}
		}
	}
	for (int i = 0; i < 6; i++) printf("period %3d  difference %.1f\n", bestP[i], best[i]);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 5 && !strcmp(argv[1], "preview")) return preview(argv[2], argv[3], argv[4], argc > 5 ? argv[5] : 0);
	if (argc == 7 && !strcmp(argv[1], "period")) return period(argv[2], atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atoi(argv[6]));
	fprintf(stderr, "usage: layouttool preview <layout> <in.bmp> <out.bmp> [views.txt]\n"
		"       layouttool period <in.bmp> <x0> <x1> <y0> <y1>\n");
	return 2;
}
