// Imperialism II HD - stage 1: scale the fixed 800x600 game view to fill the screen.
//
// Built as ddraw.dll and dropped next to the game, so the exe loads it in place of the system
// DirectDraw (every call is forwarded to the real one). Nothing on disk is patched; deleting
// ddraw.dll restores the stock game.
//
// How it works: the game paints its whole frame into an 800x600 offscreen bitmap and copies it to
// the view window. We enlarge that window, give every device context for it a GDI mapping from
// 800x600 logical units to its real size, and translate coordinates the other way for input.

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define BASE_W 800 // the width every screen was designed for
#define GAME_W g_gameW
#define GAME_H 600
#define VIEW_CHILD_ID 0xE900 // AFX_IDW_PANE_FIRST: the id MFC gives a frame's main view

// ---------------------------------------------------------------------------------------------
// Forwarding to the real ddraw.dll

#define DDRAW_EXPORTS(X) \
	X(AcquireDDThreadLock) X(CompleteCreateSysmemSurface) X(D3DParseUnknownCommand) \
	X(DDGetAttachedSurfaceLcl) X(DDInternalLock) X(DDInternalUnlock) X(DSoundHelp) \
	X(DirectDrawCreate) X(DirectDrawCreateClipper) X(DirectDrawCreateEx) \
	X(DirectDrawEnumerateA) X(DirectDrawEnumerateExA) X(DirectDrawEnumerateExW) \
	X(DirectDrawEnumerateW) X(DllCanUnloadNow) X(DllGetClassObject) X(GetDDSurfaceLocal) \
	X(GetOLEThunkData) X(GetSurfaceFromDC) X(RegisterSpecialCase) X(ReleaseDDThreadLock) \
	X(SetAppCompatData)

#define DECLARE_FORWARD(name) \
	FARPROC real_##name; \
	__attribute__((naked)) void fw_##name(void) { __asm__("jmp *_real_" #name); }
DDRAW_EXPORTS(DECLARE_FORWARD)

static void load_real_ddraw(void)
{
	char path[MAX_PATH];
	GetSystemDirectoryA(path, sizeof path);
	strcat(path, "\\ddraw.dll");
	HMODULE real = LoadLibraryA(path);
#define RESOLVE_FORWARD(name) real_##name = real ? GetProcAddress(real, #name) : NULL;
	DDRAW_EXPORTS(RESOLVE_FORWARD)
}

// ---------------------------------------------------------------------------------------------
// State

static int g_gameW = BASE_W; // width of the game's frame buffer; wider than BASE_W in wide mode
// Screens other than the map stay 800 wide. In wide mode they are shown centred by shifting
// the picture this many game pixels to the right when it is presented; the game itself still
// sees them at x=0, so its cached click regions stay valid.
static int g_offsetX;
static HWND g_frame, g_view;

// HD artwork layer (defined further down).
static BOOL g_hd;
static HDC g_hdDC;
static BOOL hd_compose(HDC source, int x0, int y0, int x1, int y1);
static void __attribute__((thiscall)) hd_copy_pixels(BYTE *src, BYTE *dst, int sx, int sy, int w, int h,
	int dx, int dy, DWORD transparent);
static WNDPROC g_frameProc, g_viewProc;
static int g_viewW = BASE_W, g_viewH = GAME_H; // current size of the view window, in pixels
static BOOL g_smooth = TRUE, g_integerScale = FALSE;
static char g_logPath[MAX_PATH];

static void logf_(const char *fmt, ...)
{
	FILE *f = fopen(g_logPath, "a");
	if (!f) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

static BOOL scaled(void) { return g_view && (g_viewW != GAME_W || g_viewH != GAME_H); }

static int to_game_x(int x) { return MulDiv(x, GAME_W, g_viewW); }
static int to_game_y(int y) { return MulDiv(y, GAME_H, g_viewH); }
static int to_view_x(int x) { return MulDiv(x, g_viewW, GAME_W); }
static int to_view_y(int y) { return MulDiv(y, g_viewH, GAME_H); }

// Game-space rect -> view pixels, grown by a pixel so rounding never leaves a stale edge.
static void rect_to_view(RECT *r)
{
	r->left = to_view_x(r->left) - 1;
	r->top = to_view_y(r->top) - 1;
	r->right = to_view_x(r->right) + 1;
	r->bottom = to_view_y(r->bottom) + 1;
}

// Where the view sits inside the frame's client area: as large as fits, centred, 4:3.
static void view_placement(RECT *out)
{
	RECT c;
	GetClientRect(g_frame, &c);
	double s = min((double)c.right / GAME_W, (double)c.bottom / GAME_H);
	if (g_integerScale) s = (double)(int)s;
	if (s < 1.0) s = 1.0;
	int w = (int)(GAME_W * s + 0.5), h = (int)(GAME_H * s + 0.5);
	out->left = (c.right - w) / 2;
	out->top = (c.bottom - h) / 2;
	out->right = out->left + w;
	out->bottom = out->top + h;
}

static void place_view(void)
{
	RECT r;
	view_placement(&r);
	SetWindowPos(g_view, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
		SWP_NOZORDER | SWP_NOACTIVATE);
}

static void map_dc(HDC dc, HWND hwnd)
{
	SetMapMode(dc, MM_ANISOTROPIC);
	SetWindowExtEx(dc, GAME_W, GAME_H, NULL);
	SetViewportExtEx(dc, g_viewW, g_viewH, NULL);
	SetViewportOrgEx(dc, hwnd == g_view ? MulDiv(g_offsetX, g_viewW, GAME_W) : 0, 0, NULL);
	SetStretchBltMode(dc, g_smooth ? HALFTONE : COLORONCOLOR);
	SetBrushOrgEx(dc, 0, 0, NULL);
}

// Pop-ups (the pause menu, message boxes, confirmations) are separate 'AmbitMcWindow' windows the
// game puts on the frame, beside the view. They are drawn and hit-tested exactly like the view,
// so they get the same treatment: same scale factor, their own origin.
#define POPUP_CLASS "AmbitMcWindow"
static HWND g_popups[16];

static BOOL is_popup(HWND hwnd)
{
	if (!hwnd) return FALSE;
	for (int i = 0; i < (int)(sizeof g_popups / sizeof g_popups[0]); i++) if (g_popups[i] == hwnd) return TRUE;
	return FALSE;
}

// A window whose contents are in game coordinates.
static BOOL is_game_window(HWND hwnd) { return hwnd && (hwnd == g_view || is_popup(hwnd)); }

// ---------------------------------------------------------------------------------------------
// Window procedures layered over the game's

// Windows the game places on top of the view (the movie player, text fields) are positioned in
// game coordinates by whoever moves them; scale every move and resize on its way through.
static const char kChildProcProp[] = "imp2hd.proc";

// Each child's rectangle as the game last asked for it, in game coordinates, so it can be laid
// out again when the view changes size (the movie window is set up before the view is enlarged).
// "centred" marks the movie player: it is placed in the middle of the frame whatever position
// the game asks for, because the game centres it against a width that depends on the mode.
static struct { HWND hwnd; int x, y, cx, cy; BOOL centred; } g_children[16];

static int child_slot(HWND hwnd, BOOL create)
{
	int free_ = -1;
	for (int i = 0; i < (int)(sizeof g_children / sizeof g_children[0]); i++) {
		if (g_children[i].hwnd == hwnd) return i;
		if (!g_children[i].hwnd && free_ < 0) free_ = i;
	}
	if (create && free_ >= 0) g_children[free_].hwnd = hwnd;
	return create ? free_ : -1;
}

// Text fields are real Windows controls drawing their own text, so enlarging the box is not
// enough: give them an enlarged copy of whatever font the game sets. The game still sees its own
// font handle when it asks for it back.
static const char kGameFontProp[] = "imp2hd.font", kScaledFontProp[] = "imp2hd.sfont";

static void drop_scaled_font(HWND hwnd)
{
	HFONT old = (HFONT)RemovePropA(hwnd, kScaledFontProp);
	if (old) DeleteObject(old);
	RemovePropA(hwnd, kGameFontProp);
}

static HFONT scaled_font_for(HWND hwnd, HFONT gameFont)
{
	LOGFONTA lf;
	HFONT base = gameFont ? gameFont : (HFONT)GetStockObject(SYSTEM_FONT);
	if (!GetObjectA(base, sizeof lf, &lf)) return gameFont;
	lf.lfHeight = MulDiv(lf.lfHeight, g_viewH, GAME_H);
	lf.lfWidth = MulDiv(lf.lfWidth, g_viewW, GAME_W);
	HFONT made = CreateFontIndirectA(&lf);
	if (!made) return gameFont;
	drop_scaled_font(hwnd);
	SetPropA(hwnd, kGameFontProp, (HANDLE)gameFont);
	SetPropA(hwnd, kScaledFontProp, (HANDLE)made);
	return made;
}

static LRESULT CALLBACK child_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	WNDPROC prev = (WNDPROC)GetPropA(hwnd, kChildProcProp);
	if (msg == WM_SETFONT && scaled()) wp = (WPARAM)scaled_font_for(hwnd, (HFONT)wp);
	if (msg == WM_GETFONT && GetPropA(hwnd, kScaledFontProp)) return (LRESULT)GetPropA(hwnd, kGameFontProp);
	if (msg == WM_NCDESTROY) drop_scaled_font(hwnd);
	if (msg == WM_WINDOWPOSCHANGING) {
		WINDOWPOS *pos = (WINDOWPOS *)lp;
		int i = child_slot(hwnd, FALSE);
		if (i >= 0) {
			if (!(pos->flags & SWP_NOMOVE)) { g_children[i].x = pos->x; g_children[i].y = pos->y; }
			if (!(pos->flags & SWP_NOSIZE)) { g_children[i].cx = pos->cx; g_children[i].cy = pos->cy; }
		}
		if (scaled()) {
			int shift = GetParent(hwnd) == g_view ? g_offsetX : 0;
			if (!(pos->flags & SWP_NOMOVE)) {
				int gameX = pos->x + shift;
				if (i >= 0 && g_children[i].centred) gameX = (GAME_W - g_children[i].cx) / 2;
				pos->x = to_view_x(gameX);
				pos->y = to_view_y(pos->y);
			}
			if (!(pos->flags & SWP_NOSIZE)) { pos->cx = to_view_x(pos->cx); pos->cy = to_view_y(pos->cy); }
		}
	}
	if (msg == WM_NCDESTROY) {
		int i = child_slot(hwnd, FALSE);
		if (i >= 0) g_children[i].hwnd = NULL;
		RemovePropA(hwnd, kChildProcProp);
	}
	return CallWindowProcA(prev, hwnd, msg, wp, lp);
}

static void relayout_children(void)
{
	for (int i = 0; i < (int)(sizeof g_children / sizeof g_children[0]); i++) {
		if (!g_children[i].hwnd) continue;
		SetWindowPos(g_children[i].hwnd, NULL, g_children[i].x, g_children[i].y, g_children[i].cx,
			g_children[i].cy, SWP_NOZORDER | SWP_NOACTIVATE);
	}
}

static void adopt_child(HWND parent, HWND child)
{
	if (GetPropA(child, kChildProcProp)) return;
	char cls[64] = "";
	GetClassNameA(child, cls, sizeof cls);
	int slot = child_slot(child, TRUE);
	if (slot >= 0) g_children[slot].centred = g_gameW > BASE_W && lstrcmpiA(cls, "MCIWndClass") == 0;
	SetPropA(child, kChildProcProp, (HANDLE)SetWindowLongA(child, GWL_WNDPROC, (LONG)child_proc));

	// It was created at game coordinates; re-apply them so they pass through child_proc.
	RECT r;
	GetWindowRect(child, &r);
	MapWindowPoints(NULL, parent, (POINT *)&r, 2);
	logf_("child %p of %p class '%s' style %08lx at %ld,%ld %ldx%ld", (void *)child, (void *)parent, cls,
		(unsigned long)GetWindowLongA(child, GWL_STYLE), r.left, r.top, r.right - r.left, r.bottom - r.top);
	SetWindowPos(child, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);

	// Re-send whatever font it has now, so it is swapped for the enlarged one.
	if (lstrcmpiA(cls, "Edit") == 0 || lstrcmpiA(cls, "Static") == 0 || lstrcmpiA(cls, "Button") == 0 ||
		lstrcmpiA(cls, "ListBox") == 0 || lstrcmpiA(cls, "ComboBox") == 0) {
		SendMessageA(child, WM_SETFONT, (WPARAM)SendMessageA(child, WM_GETFONT, 0, 0), TRUE);
	}
}

static void view_placement(RECT *out);

// The game positions a pop-up at (view origin + game offset) in frame coordinates and sizes it in
// game pixels. Keep the origin, scale the offset and the size.
static void popup_rect_to_frame(int *x, int *y, int *cx, int *cy)
{
	RECT v;
	view_placement(&v);
	// The game centres pop-ups for an 800-wide layout; shift them by half the extra width.
	if (x) *x = v.left + to_view_x(*x - v.left + (g_gameW - BASE_W) / 2);
	if (y) *y = v.top + to_view_y(*y - v.top);
	if (cx) *cx = to_view_x(*cx);
	if (cy) *cy = to_view_y(*cy);
}

static LRESULT CALLBACK popup_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	WNDPROC prev = (WNDPROC)GetPropA(hwnd, kChildProcProp);
	switch (msg) {
	case WM_WINDOWPOSCHANGING:
		if (scaled()) {
			WINDOWPOS *pos = (WINDOWPOS *)lp;
			if (!(pos->flags & SWP_NOMOVE)) popup_rect_to_frame(&pos->x, &pos->y, NULL, NULL);
			if (!(pos->flags & SWP_NOSIZE)) popup_rect_to_frame(NULL, NULL, &pos->cx, &pos->cy);
		}
		break;
	case WM_SIZE:
		if (scaled()) lp = MAKELPARAM(to_game_x(LOWORD(lp)), to_game_y(HIWORD(lp)));
		break;
	case WM_PARENTNOTIFY:
		if (LOWORD(wp) == WM_CREATE) adopt_child(hwnd, (HWND)lp);
		break;
	case WM_NCDESTROY:
		for (int i = 0; i < (int)(sizeof g_popups / sizeof g_popups[0]); i++) if (g_popups[i] == hwnd) g_popups[i] = NULL;
		RemovePropA(hwnd, kChildProcProp);
		break;
	default:
		if (msg >= WM_MOUSEFIRST && msg <= WM_MBUTTONDBLCLK && scaled()) {
			lp = MAKELPARAM(to_game_x((short)LOWORD(lp)), to_game_y((short)HIWORD(lp)));
		}
		break;
	}
	return CallWindowProcA(prev, hwnd, msg, wp, lp);
}

static void adopt_popup(HWND hwnd)
{
	for (int i = 0; i < (int)(sizeof g_popups / sizeof g_popups[0]); i++) {
		if (g_popups[i]) continue;
		g_popups[i] = hwnd;
		SetPropA(hwnd, kChildProcProp, (HANDLE)SetWindowLongA(hwnd, GWL_WNDPROC, (LONG)popup_proc));
		return;
	}
}

// ---------------------------------------------------------------------------------------------
// Diagnostics: dump the game's view tree
//
// Screens are trees of TView objects (McAppUI.cpp). Layout fields, from the decompilation:
//   +0x1c four-character id   +0x20 parent view   +0x24 location in parent (x, y)
//   +0x2c location in window (x, y)   +0x34 size (w, h)   +0x44 CList of subviews
// The application object (global at 0x00759888) keeps the map screen's root picture at +0x48.

#define VIEW_DUMP_TIMER 0x1D2D
static BOOL g_knownBuild, g_dumpViews;
static void widen_globals(void);
static int g_wideSteps = 7;
static int g_traceBudget; // diagnostic: number of paint calls still to be logged
static BOOL g_wideMap; // tile cache relocated, so the map view itself may be widened

static BOOL readable(const void *p, size_t n) { return p && !IsBadReadPtr(p, n); }

// MFC classes answer GetRuntimeClass (vtable slot 0) with "mov eax, <CRuntimeClass*>; ret", and
// the class name is the first field. Read it out of the code instead of calling into the game.
static const char *view_class(const BYTE *view)
{
	if (!readable(view, 4)) return "?";
	const BYTE **vtable = *(const BYTE ***)view;
	if (!readable(vtable, 4)) return "?";
	const BYTE *fn = vtable[0];
	for (int hops = 0; hops < 2 && readable(fn, 6) && fn[0] == 0xe9; hops++) fn += 5 + *(const int *)(fn + 1);
	if (!readable(fn, 6) || fn[0] != 0xb8 || fn[5] != 0xc3) return "?";
	const char **rtc = *(const char ***)(fn + 1);
	return readable(rtc, 4) && readable(*rtc, 2) ? *rtc : "?";
}

static void dump_view(const BYTE *v, int depth, int *budget)
{
	if (depth > 12 || --*budget < 0 || !readable(v, 0x54)) return;
	const int *f = (const int *)v;
	DWORD id = (DWORD)f[0x1c / 4];
	char name[5] = { (char)(id >> 24), (char)(id >> 16), (char)(id >> 8), (char)id, 0 };
	for (int i = 0; i < 4; i++) if (name[i] < 32 || name[i] > 126) name[i] = '.';
	logf_("%*s%s '%s' at %d,%d (window %d,%d) size %dx%d  [%p]", depth * 2, "", view_class(v), name,
		f[0x24 / 4], f[0x28 / 4], f[0x2c / 4], f[0x30 / 4], f[0x34 / 4], f[0x38 / 4], (void *)v);

	const BYTE *list = *(const BYTE **)(v + 0x44);
	if (!readable(list, 8)) return;
	for (const void **node = *(const void ***)(list + 4); readable(node, 12); node = (const void **)node[0]) {
		dump_view((const BYTE *)node[2], depth + 1, budget);
	}
}

// ---------------------------------------------------------------------------------------------
// Wide mode: re-laying out the map screen
//
// The map screen ('main') is built for 800x600: a 9x8-tile map ('DLOG', 576x512) on the left and
// the unit/minimap panel ('tool') on the right. In wide mode the extra width goes to the map and
// everything to its right shifts over.

#define ID4(a, b, c, d) (((DWORD)(a) << 24) | ((DWORD)(b) << 16) | ((DWORD)(c) << 8) | (DWORD)(d))
#define VIEW_INT(view, offset) (*(int *)((BYTE *)(view) + (offset)))
enum { V_ID = 0x1c, V_PARENT = 0x20, V_X = 0x24, V_Y = 0x28, V_W = 0x34, V_H = 0x38, V_SUBVIEWS = 0x44 };

static BYTE *child_view(BYTE *view, DWORD id)
{
	BYTE *list = *(BYTE **)(view + V_SUBVIEWS);
	if (!readable(list, 8)) return NULL;
	for (void **node = *(void ***)(list + 4); readable(node, 12); node = (void **)node[0]) {
		BYTE *child = (BYTE *)node[2];
		if (readable(child, 0x54) && (DWORD)VIEW_INT(child, V_ID) == id) return child;
	}
	return NULL;
}

// TView::UpdateCoordinates (vtable slot 0x168): recomputes window positions down the tree.
static void update_coordinates(BYTE *view)
{
	typedef void (__attribute__((thiscall)) *Method)(void *);
	((Method)(*(void ***)view)[0x168 / 4])(view);
}

// The map screen's background is one 800x600 picture: border art, a hole for the map, and the
// side panel's artwork from about x=586 on. For a wider screen the right-hand 256 columns move to
// the new right edge with the panel, and the gap in the top and bottom borders is filled by
// repeating a plain 64-pixel slice of them. (The gap's middle rows are covered by the map.)
#define BG_SPLIT 544      // columns from here on belong to the right-hand side
#define BG_SLICE 480      // start of the slice repeated to fill the gap
#define BG_SLICE_W 64
// The land map view in the 800x600 layout.
#define MAP_HOLE_X 10
#define MAP_HOLE_Y 58
#define MAP_HOLE_W 576
#define MAP_HOLE_H 512
// The toolbar ribbon painted into the bottom border, tails included.
#define RIBBON_LEFT 112
#define RIBBON_RIGHT 488

static void *g_backgroundVtable[0x101];

// One horizontal run of the widened background: destination columns [dstStart, dstEnd) come from
// the picture starting at srcStart. With tile > 0 the source is a strip that wide, repeated.
typedef struct { int dstStart, dstEnd, srcStart, tile; } BgSpan;

static void __attribute__((thiscall)) wide_background_draw(BYTE *self, const int *area)
{
	typedef void (__attribute__((thiscall)) *CopyPixels)(void *src, void *dst, int sx, int sy, int w, int h, int dx, int dy, DWORD transparent);
	typedef void *(__cdecl *QuickOffscreenDib)(void);
	void *src = *(void **)(self + 0x90); // TPicture: the loaded picture
	void *dst = ((QuickOffscreenDib)0x004a8540)();
	if (!src || !dst) return;

	int extra = g_gameW - BASE_W;
	// Top border and side columns: everything from BG_SPLIT on moves right with the panel.
	const BgSpan upper[] = {
		{ 0, BG_SPLIT, 0, 0 },
		{ BG_SPLIT, BG_SPLIT + extra, BG_SLICE, BG_SLICE_W },
		{ BG_SPLIT + extra, g_gameW, BG_SPLIT, 0 },
	};
	// Bottom border: the toolbar ribbon is moved half the extra width so it stays centred under
	// the map, with plain border either side of it.
	const BgSpan lower[] = {
		{ 0, RIBBON_LEFT, 0, 0 },
		{ RIBBON_LEFT, RIBBON_LEFT + extra / 2, RIBBON_RIGHT, BG_SPLIT - RIBBON_RIGHT },
		{ RIBBON_LEFT + extra / 2, RIBBON_RIGHT + extra / 2, RIBBON_LEFT, 0 },
		{ RIBBON_RIGHT + extra / 2, BG_SPLIT + extra, RIBBON_RIGHT, BG_SPLIT - RIBBON_RIGHT },
		{ BG_SPLIT + extra, g_gameW, BG_SPLIT, 0 },
	};

	// The map's rectangle is never painted: the game does not redraw the map after the
	// background, it relies on the background leaving that area alone.
	const int holeLeft = MAP_HOLE_X, holeRight = MAP_HOLE_X + MAP_HOLE_W + extra;
	const int bands[3][2] = { { 0, MAP_HOLE_Y }, { MAP_HOLE_Y, MAP_HOLE_Y + MAP_HOLE_H }, { MAP_HOLE_Y + MAP_HOLE_H, GAME_H } };

	for (int band = 0; band < 3; band++) {
		int top = max(area[1], bands[band][0]), bottom = min(area[3], bands[band][1]);
		if (bottom <= top) continue;
		const BgSpan *spans = band == 2 ? lower : upper;
		int count = band == 2 ? (int)(sizeof lower / sizeof lower[0]) : (int)(sizeof upper / sizeof upper[0]);

		for (int i = 0; i < count; i++) {
			for (int dx = spans[i].dstStart; dx < spans[i].dstEnd;) {
				// One straight copy, or one repeat of the tile.
				int into = spans[i].tile ? (dx - spans[i].dstStart) % spans[i].tile : dx - spans[i].dstStart;
				int w = spans[i].tile ? min(spans[i].tile - into, spans[i].dstEnd - dx) : spans[i].dstEnd - dx;
				int sx = spans[i].srcStart + into;

				// Up to two pieces of this run survive: left of the hole and right of it.
				for (int side = 0; side < 2; side++) {
					int left = max(dx, area[0]), right = min(dx + w, area[2]);
					if (band == 1) {
						if (side == 0) right = min(right, holeLeft);
						else left = max(left, holeRight);
					} else if (side == 1) {
						break;
					}
					if (right > left) {
						// Through the HD layer when it is on, so the widened background gets HD art too.
						CopyPixels copy = g_hd ? (CopyPixels)hd_copy_pixels : (CopyPixels)0x00487790;
						copy(src, dst, sx + (left - dx), top, right - left, bottom - top, left, top, 0xffffffff);
					}
				}
				dx += w;
			}
		}
	}
}
// Gives this one picture object its own copy of its class's virtual table with Draw replaced.
static BOOL hook_background_draw(BYTE *picture)
{
	const DWORD originalDraw = 0x005c8800; // TOffLimitsPicture::Draw
	void **vtable = *(void ***)picture;
	if (vtable == &g_backgroundVtable[1]) return TRUE;
	if (!readable(vtable - 1, sizeof g_backgroundVtable)) return FALSE;

	for (int slot = 0; slot < 0x100; slot++) {
		const BYTE *fn = (const BYTE *)vtable[slot];
		// Incremental-link builds reach functions through a "jmp" thunk.
		if (readable(fn, 5) && fn[0] == 0xe9) fn += 5 + *(const int *)(fn + 1);
		if ((DWORD)(DWORD_PTR)fn != originalDraw) continue;
		memcpy(g_backgroundVtable, vtable - 1, sizeof g_backgroundVtable);
		g_backgroundVtable[slot + 1] = (void *)wide_background_draw;
		*(void ***)picture = &g_backgroundVtable[1];
		logf_("background draw replaced (vtable slot %d)", slot);
		return TRUE;
	}
	return FALSE;
}

static void widen_map_screen(BYTE *picture)
{
	int extra = g_gameW - BASE_W;
	BYTE *map = child_view(picture, ID4('D', 'L', 'O', 'G'));
	if (extra <= 0 || !map || VIEW_INT(picture, V_W) != BASE_W) return;

	// g_wideSteps is a debugging aid: each bit enables one part of the re-layout.
	if (g_wideSteps & 1) {
		// The picture is only widened if its drawing can be replaced: left alone at 1056 it
		// reads past each 800-pixel row of its bitmap.
		BOOL background = hook_background_draw(picture);
		for (BYTE *v = picture; readable(v, 0x54); v = *(BYTE **)(v + V_PARENT)) {
			if (v == picture && !background) continue;
			if (VIEW_INT(v, V_W) == BASE_W) VIEW_INT(v, V_W) = g_gameW;
		}
	}
	if (g_wideSteps & 2) {
		static const DWORD shifted[] = { ID4('t', 'o', 'o', 'l'), ID4('s', 'e', 'n', 'd'), ID4('t', 'b', 'r', '4') };
		for (int i = 0; i < (int)(sizeof shifted / sizeof shifted[0]); i++) {
			BYTE *v = child_view(picture, shifted[i]);
			if (!v) continue;
			VIEW_INT(v, V_X) += extra;
			update_coordinates(v); // only walks down when the view's own position changed
		}
		// Hidden views are parked at x=1001, just past the right edge of an 800-wide screen.
		// On a wider screen that is visible, and the idle sea map draws its blank contents over
		// the panel and into the buffer it shares with the land map. Park it further out.
		BYTE *ocean = child_view(picture, ID4('D', 'O', 'O', 'G'));
		logf_("parking point for hidden map views: %d,%d", *(int *)0x007608f8, *(int *)0x007608fc);
		if (ocean && g_wideMap && (g_wideSteps & 4)) VIEW_INT(ocean, V_W) += extra; // the zoomed-out overview
		if (ocean && VIEW_INT(ocean, V_X) >= BASE_W) {
			VIEW_INT(ocean, V_X) += extra;
			update_coordinates(ocean);
		}
		// Top status bar: the date stays at the left, the treasury moves right in proportion to
		// the longer bar, and the message text in the middle stays centred over the map.
		BYTE *status = child_view(picture, ID4('t', 'b', 'r', '1'));
		BYTE *treasury = status ? child_view(status, ID4('t', 'r', 'e', 'a')) : NULL;
		if (treasury) {
			VIEW_INT(status, V_W) += extra / 2; // or the treasury would fall outside its parent
			VIEW_INT(treasury, V_X) += extra * 3 / 8;
			update_coordinates(treasury);
		}
		BYTE *message = child_view(picture, ID4('c', 'u', 'r', 's'));
		if (message) {
			VIEW_INT(message, V_X) += extra / 2;
			update_coordinates(message);
		}
		// The bottom toolbar's buttons are painted into the background's ribbon; both move half
		// the extra width so they stay centred under the map.
		BYTE *toolbar = child_view(picture, ID4('t', 'o', 'p', 'B'));
		if (toolbar) {
			VIEW_INT(toolbar, V_X) += extra / 2;
			update_coordinates(toolbar);
		}
	}
	if ((g_wideSteps & 4) && g_wideMap) {
		VIEW_INT(map, V_W) += extra;

		// The background picture leaves a hole where the map goes (TOffLimitsPicture); the hole
		// was cut for the old map size when the screen was built.
		typedef void **(__cdecl *NewRgnFn)(void);
		typedef void (__cdecl *SetRectRgnFn)(void **, short, short, short, short);
		typedef void (__attribute__((thiscall)) *SetRgnFn)(void *, void **);
		typedef void **(__cdecl *DisposeRgnFn)(void **);
		void **rgn = ((NewRgnFn)0x004a94e0)();
		((SetRectRgnFn)0x004ac6c0)(rgn, (short)VIEW_INT(map, V_X), (short)VIEW_INT(map, V_Y),
			(short)(VIEW_INT(map, V_X) + VIEW_INT(map, V_W)), (short)(VIEW_INT(map, V_Y) + VIEW_INT(map, V_H)));
		((SetRgnFn)0x005c88b0)(picture, rgn);
		((DisposeRgnFn)0x004a92d0)(rgn);
	}

	if (g_wideSteps & 8) {
		// Experiment: repaint through the game's own TView::InvalidateRect on the map view only.
		typedef void (__attribute__((thiscall)) *Invalidate)(void *, const RECT *, int);
		((Invalidate)0x0049fbf0)(map, NULL, 0);
	} else {
		InvalidateRect(g_view, NULL, FALSE);
	}
	logf_("map screen widened by %d (steps %d)", extra, g_wideSteps);
}

// Every screen is a child of the 'base' view inside the game window's view tree:
//   TGameWindow 'WIND' > TIncludeView 'Incl' > TView 'base' > one picture per screen.
// The game window hangs off the object at 0x0075a60c.
static BYTE *screens_container(void)
{
	BYTE **holder = (BYTE **)0x0075a60c;
	if (!g_knownBuild || !readable(*holder, 8)) return NULL;
	BYTE *root = *(BYTE **)(*holder + 4);
	if (!readable(root, 0x54)) return NULL;
	BYTE *include = child_view(root, ID4('I', 'n', 'c', 'l'));
	// The containers are left at 800 wide here; widen_map_screen widens them when the map needs it.
	return include ? child_view(include, ID4('b', 'a', 's', 'e')) : NULL;
}

static BOOL is_map_screen(const BYTE *screen) { return strcmp(view_class(screen), "TMapUberPicture") == 0; }


// Called before each paint and from a timer. The map screen is widened when it is built; every
// other screen was designed for 800 and is presented centred instead (see g_offsetX).
static void watch_screens(void)
{
	static BYTE *handled[32];
	static int handledCount;
	if (g_gameW <= BASE_W && !g_dumpViews) return;
	BYTE *base = screens_container();

	BYTE *current[32], *top = NULL;
	int count = 0;
	BYTE *list = base ? *(BYTE **)(base + V_SUBVIEWS) : NULL;
	if (readable(list, 8)) {
		for (void **node = *(void ***)(list + 4); readable(node, 12) && count < 32; node = (void **)node[0]) {
			BYTE *screen = (BYTE *)node[2];
			if (!readable(screen, 0x54)) continue;
			top = screen;

			BOOL seen = FALSE;
			for (int i = 0; i < handledCount; i++) seen |= handled[i] == screen;
			if (!seen) {
				BOOL map = is_map_screen(screen);
				// The map view is one of the last things attached; wait until the screen is built.
				if (map && !child_view(screen, ID4('D', 'L', 'O', 'G'))) continue;

				if (g_gameW > BASE_W) {
					if (map) widen_map_screen(screen);
				}
				if (g_dumpViews) g_traceBudget = 60;
				DWORD id = (DWORD)VIEW_INT(screen, V_ID);
				logf_("screen %s '%c%c%c%c' at %d,%d size %dx%d", view_class(screen), (char)(id >> 24),
					(char)(id >> 16), (char)(id >> 8), (char)id, VIEW_INT(screen, V_X), VIEW_INT(screen, V_Y),
					VIEW_INT(screen, V_W), VIEW_INT(screen, V_H));
				if (g_dumpViews) {
					int budget = 400;
					dump_view(screen, 1, &budget);
				}
			}
			current[count++] = screen;
		}
	}
	memcpy(handled, current, sizeof(BYTE *) * count);
	handledCount = count;
	// Whatever is on top decides how the frame is presented. Only the map uses the full width;
	// with no screen at all (the intro movies) the 800-wide layout is centred too.
	int offset = g_gameW > BASE_W && !(top && is_map_screen(top)) ? (g_gameW - BASE_W) / 2 : 0;
	if (offset != g_offsetX) {
		g_offsetX = offset;
		relayout_children();
		InvalidateRect(g_view, NULL, FALSE);
	}
}
static LRESULT CALLBACK view_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_TIMER && wp == VIEW_DUMP_TIMER) {
		watch_screens();
		return 0;
	}
	switch (msg) {
	case WM_PARENTNOTIFY:
		if (LOWORD(wp) == WM_CREATE) adopt_child(hwnd, (HWND)lp);
		break;
	case WM_WINDOWPOSCHANGING: {
		// The game keeps trying to size the view to 800x600; hold it at our placement.
		WINDOWPOS *pos = (WINDOWPOS *)lp;
		RECT r;
		view_placement(&r);
		if (!(pos->flags & SWP_NOMOVE)) { pos->x = r.left; pos->y = r.top; }
		if (!(pos->flags & SWP_NOSIZE)) { pos->cx = r.right - r.left; pos->cy = r.bottom - r.top; }
		break;
	}
	case WM_SIZE:
		g_viewW = max(1, LOWORD(lp));
		g_viewH = max(1, HIWORD(lp));
		logf_("view size %dx%d", g_viewW, g_viewH);
		relayout_children();
		InvalidateRect(hwnd, NULL, FALSE);
		lp = MAKELPARAM(GAME_W, GAME_H);
		break;
	case WM_NCDESTROY: {
		WNDPROC prev = g_viewProc;
		g_view = NULL;
		g_viewW = GAME_W;
		g_viewH = GAME_H;
		return CallWindowProcA(prev, hwnd, msg, wp, lp);
	}
	default:
		if (msg >= WM_MOUSEFIRST && msg <= WM_MBUTTONDBLCLK && scaled()) {
			int rawX = (short)LOWORD(lp), rawY = (short)HIWORD(lp);
			lp = MAKELPARAM(to_game_x(rawX) - g_offsetX, to_game_y(rawY));
			if (g_dumpViews && (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)) {
				logf_("%s at view %d,%d -> game %d,%d (offset %d)", msg == WM_LBUTTONDOWN ? "press" : "release",
					rawX, rawY, (short)LOWORD(lp), (short)HIWORD(lp), g_offsetX);
			}
		}
		break;
	}
	return CallWindowProcA(g_viewProc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK frame_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_ERASEBKGND && g_view) {
		// The game only clears an 800x600 corner; black out everything around the view.
		RECT c, v;
		GetClientRect(hwnd, &c);
		GetWindowRect(g_view, &v);
		MapWindowPoints(NULL, hwnd, (POINT *)&v, 2);
		HDC dc = (HDC)wp;
		int saved = SaveDC(dc);
		ExcludeClipRect(dc, v.left, v.top, v.right, v.bottom);
		FillRect(dc, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
		RestoreDC(dc, saved);
		return 1;
	}
	LRESULT res = CallWindowProcA(g_frameProc, hwnd, msg, wp, lp);
	if (msg == WM_SIZE && g_view && wp != SIZE_MINIMIZED) place_view();
	if (msg == WM_NCDESTROY) g_frame = NULL;
	return res;
}

// ---------------------------------------------------------------------------------------------
// Hooked Win32 calls (the exe's import table is pointed at these)

static HWND (WINAPI *o_CreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static HDC (WINAPI *o_BeginPaint)(HWND, PAINTSTRUCT *);
static HDC (WINAPI *o_GetDC)(HWND);
static BOOL (WINAPI *o_GetClientRect)(HWND, RECT *);
static BOOL (WINAPI *o_ScreenToClient)(HWND, POINT *);
static BOOL (WINAPI *o_ClientToScreen)(HWND, POINT *);
static BOOL (WINAPI *o_InvalidateRect)(HWND, const RECT *, BOOL);
static BOOL (WINAPI *o_ValidateRect)(HWND, const RECT *);
static BOOL (WINAPI *o_RedrawWindow)(HWND, const RECT *, HRGN, UINT);
static BOOL (WINAPI *o_BitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);

static HWND WINAPI h_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y,
	int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
	BOOL popup = g_frame && parent == g_frame && (style & WS_CHILD) && HIWORD((DWORD_PTR)cls) &&
		lstrcmpA(cls, POPUP_CLASS) == 0;
	if (popup) {
		logf_("pop-up requested at %d,%d %dx%d", x, y, w, h);
		if (scaled()) popup_rect_to_frame(&x, &y, &w, &h);
	}
	HWND hwnd = o_CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
	if (hwnd && popup) adopt_popup(hwnd);
	if (hwnd && !g_view && (style & WS_CHILD) && parent && (UINT_PTR)menu == VIEW_CHILD_ID &&
		!(GetWindowLongA(parent, GWL_STYLE) & WS_CHILD)) {
		g_frame = parent;
		g_view = hwnd;
		g_frameProc = (WNDPROC)SetWindowLongA(g_frame, GWL_WNDPROC, (LONG)frame_proc);
		g_viewProc = (WNDPROC)SetWindowLongA(g_view, GWL_WNDPROC, (LONG)view_proc);
		logf_("attached: frame %p view %p", (void *)g_frame, (void *)g_view);
		widen_globals();
		if (g_dumpViews || g_gameW > BASE_W) SetTimer(g_view, VIEW_DUMP_TIMER, 200, NULL);
		place_view();
	}
	return hwnd;
}

// With a centred screen the drawing origin is shifted, so a repaint rectangle that reaches into
// the black margins comes out with coordinates below 0 or past 800. The game skips redrawing
// for such rectangles, so hand it only the part that lies on the screen itself.
static void clamp_to_screen(RECT *r)
{
	if (!g_offsetX) return;
	RECT screen = { 0, 0, BASE_W, GAME_H }, out;
	if (IntersectRect(&out, r, &screen)) *r = out;
	else SetRectEmpty(r);
}

static int (WINAPI *o_GetClipBox)(HDC, RECT *);

static int WINAPI h_GetClipBox(HDC dc, RECT *r)
{
	int kind = o_GetClipBox(dc, r);
	if (kind != ERROR && r && g_offsetX && scaled() && WindowFromDC(dc) == g_view) clamp_to_screen(r);
	return kind;
}

static HDC WINAPI h_BeginPaint(HWND hwnd, PAINTSTRUCT *ps)
{
	// Re-lay out a freshly built map screen before its first paint, so it never shows at the
	// old width. (The timer is only a fallback.)
	if (hwnd == g_view) watch_screens();
	HDC dc = o_BeginPaint(hwnd, ps);
	if (dc && is_game_window(hwnd) && scaled()) {
		map_dc(dc, hwnd);
		DPtoLP(dc, (POINT *)&ps->rcPaint, 2);
		InflateRect(&ps->rcPaint, 1, 1);
		if (hwnd == g_view) clamp_to_screen(&ps->rcPaint);
		if (g_traceBudget > 0 && hwnd == g_view) {
			logf_("  paint rect %ld,%ld-%ld,%ld", ps->rcPaint.left, ps->rcPaint.top, ps->rcPaint.right, ps->rcPaint.bottom);
		}
	}
	return dc;
}

static HDC WINAPI h_GetDC(HWND hwnd)
{
	HDC dc = o_GetDC(hwnd);
	if (dc && is_game_window(hwnd) && scaled()) map_dc(dc, hwnd);
	return dc;
}

static BOOL WINAPI h_GetClientRect(HWND hwnd, RECT *r)
{
	if (hwnd && hwnd == g_view && scaled() && r) {
		SetRect(r, 0, 0, GAME_W, GAME_H);
		return TRUE;
	}
	BOOL ok = o_GetClientRect(hwnd, r);
	if (ok && r && is_popup(hwnd) && scaled()) { r->right = to_game_x(r->right); r->bottom = to_game_y(r->bottom); }
	return ok;
}

static BOOL WINAPI h_ScreenToClient(HWND hwnd, POINT *p)
{
	BOOL ok = o_ScreenToClient(hwnd, p);
	if (ok && is_game_window(hwnd) && scaled()) {
		p->x = to_game_x(p->x) - (hwnd == g_view ? g_offsetX : 0);
		p->y = to_game_y(p->y);
	}
	return ok;
}

static BOOL WINAPI h_ClientToScreen(HWND hwnd, POINT *p)
{
	if (p && is_game_window(hwnd) && scaled()) {
		p->x = to_view_x(p->x + (hwnd == g_view ? g_offsetX : 0));
		p->y = to_view_y(p->y);
	}
	return o_ClientToScreen(hwnd, p);
}

static BOOL WINAPI h_InvalidateRect(HWND hwnd, const RECT *r, BOOL erase)
{
	RECT v;
	if (g_traceBudget > 0 && hwnd == g_view) {
		if (r) logf_("  invalidate %ld,%ld-%ld,%ld", r->left, r->top, r->right, r->bottom);
		else logf_("  invalidate all");
	}
	if (r && is_game_window(hwnd) && scaled()) {
		v = *r;
		if (hwnd == g_view) OffsetRect(&v, g_offsetX, 0);
		rect_to_view(&v);
		r = &v;
	}
	return o_InvalidateRect(hwnd, r, erase);
}

static BOOL WINAPI h_ValidateRect(HWND hwnd, const RECT *r)
{
	RECT v;
	if (r && is_game_window(hwnd) && scaled()) {
		v = *r;
		if (hwnd == g_view) OffsetRect(&v, g_offsetX, 0);
		rect_to_view(&v);
		r = &v;
	}
	return o_ValidateRect(hwnd, r);
}

static BOOL WINAPI h_RedrawWindow(HWND hwnd, const RECT *r, HRGN rgn, UINT flags)
{
	RECT v;
	if (r && is_game_window(hwnd) && scaled()) {
		v = *r;
		if (hwnd == g_view) OffsetRect(&v, g_offsetX, 0);
		rect_to_view(&v);
		r = &v;
	}
	return o_RedrawWindow(hwnd, r, rgn, flags);
}

static BOOL holds_game_frame(HDC dc)
{
	BITMAP bm;
	HGDIOBJ obj = GetCurrentObject(dc, OBJ_BITMAP);
	return obj && GetObjectA(obj, sizeof bm, &bm) && bm.bmWidth == GAME_W && bm.bmHeight == GAME_H;
}

// The game also draws through device contexts it obtained before we attached or before the view
// was enlarged, and keeps for the whole session (its pop-up message boxes use one). Those still
// carry no mapping, or a stale one, so check the target of every blit and fix it on the spot.
static void ensure_mapped(HDC dc)
{
	if (!scaled() || !is_game_window(WindowFromDC(dc))) return;
	SIZE ext;
	POINT org;
	HWND hwnd = WindowFromDC(dc);
	int wantX = hwnd == g_view ? MulDiv(g_offsetX, g_viewW, GAME_W) : 0;
	if (GetMapMode(dc) == MM_ANISOTROPIC && GetViewportExtEx(dc, &ext) && ext.cx == g_viewW && ext.cy == g_viewH &&
		GetViewportOrgEx(dc, &org) && org.x == wantX) return;
	map_dc(dc, hwnd);
}

static BOOL (WINAPI *o_StretchBlt)(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
static int (WINAPI *o_StretchDIBits)(HDC, int, int, int, int, int, int, int, int, const void *, const BITMAPINFO *, UINT, DWORD);

static BOOL WINAPI h_StretchBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, DWORD rop)
{
	ensure_mapped(dst);
	return o_StretchBlt(dst, x, y, w, h, src, sx, sy, sw, sh, rop);
}

static int WINAPI h_StretchDIBits(HDC dst, int x, int y, int w, int h, int sx, int sy, int sw, int sh,
	const void *bits, const BITMAPINFO *info, UINT usage, DWORD rop)
{
	ensure_mapped(dst);
	if (g_traceBudget > 0 && WindowFromDC(dst) == g_view) {
		g_traceBudget--;
		logf_("  dib to view %d,%d %dx%d from %d,%d %dx%d", x, y, w, h, sx, sy, sw, sh);
	}
	return o_StretchDIBits(dst, x, y, w, h, sx, sy, sw, sh, bits, info, usage, rop);
}

static BOOL WINAPI h_BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
	ensure_mapped(dst);
	if (g_traceBudget > 0 && WindowFromDC(dst) == g_view) {
		g_traceBudget--;
		BITMAP bm = { 0 };
		HGDIOBJ obj = src ? GetCurrentObject(src, OBJ_BITMAP) : NULL;
		if (obj) GetObjectA(obj, sizeof bm, &bm);
		logf_("  blit to view %d,%d %dx%d from %d,%d of %ldx%ld bitmap", x, y, w, h, sx, sy, bm.bmWidth, bm.bmHeight);
	}

	// Reading the screen back (the blinking unit does this to composite its sprite over what is
	// already there): the enlarged view has to be shrunk into the game's small bitmap. GDI's
	// default shrink mode ANDs pixels together and turns the patch nearly black, so average them.
	if (scaled() && src && is_game_window(WindowFromDC(src))) {
		ensure_mapped(src);
		int mode = SetStretchBltMode(dst, HALFTONE);
		SetBrushOrgEx(dst, 0, 0, NULL);
		BOOL ok = o_BitBlt(dst, x, y, w, h, src, sx, sy, rop);
		SetStretchBltMode(dst, mode);
		return ok;
	}
	// Copying a piece of the frame buffer to the view: stretch the whole frame through a clip
	// instead, so every partial update lands on the same pixel grid as a full one.
	if (scaled() && src && x == sx && y == sy && WindowFromDC(dst) == g_view && holds_game_frame(src)) {
		int saved = SaveDC(dst);
		// (Widened by the margin so the black bars are repainted along with the screen's edge.)
		IntersectClipRect(dst, x - 1 - g_offsetX, y - 1, x + w + 1 + g_offsetX, y + h + 1);
		// A centred screen only owns the left 800 columns of the frame buffer; the rest still
		// holds whatever the map screen left there, so it is not shown and the margins are black.
		int width = g_offsetX ? BASE_W : GAME_W;
		BOOL ok;
		if (g_hd && rop == SRCCOPY && hd_compose(src, x - 2, y - 2, x + w + 2, y + h + 2)) {
			ok = StretchBlt(dst, 0, 0, width, GAME_H, g_hdDC, 0, 0, width * 2, GAME_H * 2, rop);
		} else {
			ok = StretchBlt(dst, 0, 0, width, GAME_H, src, 0, 0, width, GAME_H, rop);
		}
		if (g_offsetX) {
			RECT left = { -g_offsetX, 0, 0, GAME_H }, right = { BASE_W, 0, GAME_W - g_offsetX, GAME_H };
			FillRect(dst, &left, (HBRUSH)GetStockObject(BLACK_BRUSH));
			FillRect(dst, &right, (HBRUSH)GetStockObject(BLACK_BRUSH));
		}
		RestoreDC(dst, saved);
		return ok;
	}
	return o_BitBlt(dst, x, y, w, h, src, sx, sy, rop);
}

// ---------------------------------------------------------------------------------------------
// Cursors
//
// The game's own pointers (the hand over clickable areas and so on) are 1999-style masked
// cursors, 32 pixels across. Modern Windows draws those through a legacy path that can leave a
// pale box around them, and at high DPI they are tiny next to the system arrow. Each one is
// rebuilt once as an alpha-blended cursor at the system cursor size.

static HCURSOR (WINAPI *o_SetCursor)(HCURSOR);

static struct { HCURSOR from, to; } g_cursors[64];
static int g_cursorCount;

static HCURSOR rebuild_cursor(HCURSOR src)
{
	ICONINFO ii;
	BITMAP bm;
	if (!GetIconInfo(src, &ii)) return src;
	HCURSOR result = src;
	DWORD *mask = NULL, *colour = NULL, *out = NULL;
	HDC dc = GetDC(NULL);

	if (!GetObjectA(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof bm, &bm)) goto done;
	int w = bm.bmWidth;
	int maskH = ii.hbmColor ? bm.bmHeight : bm.bmHeight; // monochrome: AND plane above XOR plane
	int h = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
	if (w <= 0 || h <= 0 || w > 256 || h > 256) goto done;

	BITMAPINFO bi = { 0 };
	bi.bmiHeader.biSize = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;

	mask = malloc(w * maskH * 4);
	bi.bmiHeader.biHeight = -maskH;
	if (!mask || !GetDIBits(dc, ii.hbmMask, 0, maskH, mask, &bi, DIB_RGB_COLORS)) goto done;

	BOOL hasAlpha = FALSE;
	if (ii.hbmColor) {
		colour = malloc(w * h * 4);
		bi.bmiHeader.biHeight = -h;
		if (!colour || !GetDIBits(dc, ii.hbmColor, 0, h, colour, &bi, DIB_RGB_COLORS)) goto done;
		for (int i = 0; i < w * h && !hasAlpha; i++) hasAlpha = (colour[i] >> 24) != 0;
	}
	if (hasAlpha) goto done; // already a modern cursor (the system arrow, for instance)

	int k = max(1, GetSystemMetrics(SM_CXCURSOR) / w);
	int W = w * k, H = h * k, inverted = 0, clear = 0;
	DWORD corner = colour ? colour[0] : 0;

	bi.bmiHeader.biWidth = W;
	bi.bmiHeader.biHeight = -H;
	HBITMAP colourBmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&out, NULL, 0);
	if (!colourBmp) goto done;
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			int i = (y / k) * w + (x / k);
			BOOL transparent = (mask[i] & 0xffffff) != 0;
			DWORD rgb = colour ? (colour[i] & 0xffffff) : ((mask[i + w * h] & 0xffffff) ? 0xffffff : 0);
			DWORD px;
			if (!transparent) px = 0xff000000 | rgb;
			else if (rgb == 0) { px = 0; clear++; }
			else { px = colour ? 0 : 0xffffffff; inverted++; } // "invert the screen" has no alpha equivalent
			out[y * W + x] = px;
		}
	}
	HBITMAP maskBmp = CreateBitmap(W, H, 1, 1, NULL);
	ICONINFO made = { FALSE, ii.xHotspot * k, ii.yHotspot * k, maskBmp, colourBmp };
	HCURSOR cur = (HCURSOR)CreateIconIndirect(&made);
	DeleteObject(maskBmp);
	DeleteObject(colourBmp);
	if (cur) result = cur;
	logf_("cursor %p: %dx%d %s, %d of %d pixels transparent, %d inverting, corner %08lx mask %08lx -> %p at %dx%d",
		(void *)src, w, h, colour ? "colour" : "monochrome", clear / (k * k), w * h, inverted / (k * k),
		(unsigned long)corner, (unsigned long)mask[0], (void *)result, W, H);

done:
	free(mask);
	free(colour);
	ReleaseDC(NULL, dc);
	if (ii.hbmColor) DeleteObject(ii.hbmColor);
	if (ii.hbmMask) DeleteObject(ii.hbmMask);
	return result;
}

static HCURSOR WINAPI h_SetCursor(HCURSOR cur)
{
	if (!cur) return o_SetCursor(cur);
	for (int i = 0; i < g_cursorCount; i++) {
		if (g_cursors[i].from == cur) return o_SetCursor(g_cursors[i].to);
		if (g_cursors[i].to == cur) return o_SetCursor(cur); // one of ours, handed back to us
	}
	HCURSOR made = rebuild_cursor(cur);
	if (g_cursorCount < (int)(sizeof g_cursors / sizeof g_cursors[0])) {
		g_cursors[g_cursorCount].from = cur;
		g_cursors[g_cursorCount].to = made;
		g_cursorCount++;
	}
	return o_SetCursor(made);
}

// ---------------------------------------------------------------------------------------------
// Import-table patching

static BOOL hook_import(const char *dll, const char *name, void *hook, void *orig)
{
	BYTE *base = (BYTE *)GetModuleHandleA(NULL);
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress) return FALSE;

	for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
		if (lstrcmpiA((char *)(base + d->Name), dll) != 0 || !d->OriginalFirstThunk) continue;
		IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + d->OriginalFirstThunk);
		IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
		for (; names->u1.AddressOfData; names++, slots++) {
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
			IMAGE_IMPORT_BY_NAME *imp = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
			if (strcmp((char *)imp->Name, name) != 0) continue;
			DWORD old;
			VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
			*(void **)orig = (void *)slots->u1.Function;
			slots->u1.Function = (DWORD_PTR)hook;
			VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
			return TRUE;
		}
	}
	logf_("import not found: %s!%s", dll, name);
	return FALSE;
}

// The game's DirectDraw wrapper has one function that puts the monitor into 800x600x16
// (DDObject.cpp, 0x0048ad90 in the v1.03 exe: __thiscall, 4 stack arguments). Make it report
// success without touching the display, so the desktop stays at its native resolution.
static BOOL skip_mode_switch(void)
{
	static const BYTE expected[] = { 0x81, 0xec, 0x80, 0x00, 0x00, 0x00, 0x53, 0x55 };
	static const BYTE patch[] = { 0x33, 0xc0, 0xc2, 0x10, 0x00 }; // xor eax,eax / ret 10h
	BYTE *site = (BYTE *)0x0048ad90;

	BYTE *base = (BYTE *)GetModuleHandleA(NULL);
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
	if (base != (BYTE *)0x00400000 || nt->FileHeader.TimeDateStamp != 0x3757fa06 ||
		memcmp(site, expected, sizeof expected) != 0) {
		logf_("unrecognised exe build: display mode switch left alone");
		return FALSE;
	}
	g_knownBuild = TRUE;
	DWORD old;
	VirtualProtect(site, sizeof patch, PAGE_EXECUTE_READWRITE, &old);
	memcpy(site, patch, sizeof patch);
	VirtualProtect(site, sizeof patch, old, &old);
	FlushInstructionCache(GetCurrentProcess(), site, sizeof patch);
	return TRUE;
}

static BOOL patch_imm32(DWORD address, DWORD expected, DWORD value)
{
	DWORD *site = (DWORD *)address, old;
	if (*site != expected) {
		logf_("patch site %08lx holds %08lx, expected %08lx: skipped", (unsigned long)address,
			(unsigned long)*site, (unsigned long)expected);
		return FALSE;
	}
	VirtualProtect(site, 4, PAGE_EXECUTE_READWRITE, &old);
	*site = value;
	VirtualProtect(site, 4, old, &old);
	FlushInstructionCache(GetCurrentProcess(), site, 4);
	return TRUE;
}

static BOOL patch_imm8(DWORD address, BYTE expected, BYTE value)
{
	BYTE *site = (BYTE *)address;
	DWORD old;
	if (*site != expected) {
		logf_("patch site %08lx holds %02x, expected %02x: skipped", (unsigned long)address, *site, expected);
		return FALSE;
	}
	VirtualProtect(site, 1, PAGE_EXECUTE_READWRITE, &old);
	*site = value;
	VirtualProtect(site, 1, old, &old);
	FlushInstructionCache(GetCurrentProcess(), site, 1);
	return TRUE;
}

// The land map view (TMapDialog, with TCitySiteView and TMapEditView derived from it) caches
// rendered tiles. The bookkeeping is an array of 110 eight-byte slots embedded in the object at
// +0x84, sized for the 10x11 tiles a 576x512 view can touch; the fields after it start at +0x3f4.
// A wider view needs more slots, so the array is moved to the end of an enlarged object.
//
// Slot numbers are stored in a signed byte per map tile, which caps the count at 127. A 832-wide
// view touches 14 columns; keeping 9 rows instead of 11 makes that 126.
#define MAP_COLUMNS_WIDE 13
#define CACHE_OLD_BASE 0x84
#define CACHE_NEW_BASE 0x420   // past the largest derived class (TCitySiteView, 0x41c bytes)
#define CACHE_OLD_SLOTS 110
#define CACHE_NEW_SLOTS 127
#define CACHE_ALLOCATED_SLOTS 128 // one spare: the "no free slot" path writes one past the end

static BOOL relocate_tile_cache(void)
{
	// Address of the 32-bit displacement inside each instruction that indexes the slot array.
	static const DWORD slotAccess[] = {
		0x00559b23, 0x00559c07,                         // TMapDialog::FrameCursorArea
		0x0055b2bf, 0x0055b3a3,                         // TCitySiteView::FrameCursorArea
		0x0055cfb3,                                     // TMapDialog::FlushCache
		0x0055d021, 0x0055d02a, 0x0055d032, 0x0055d03a, // TMapDialog::DeCache
		0x0055d21d, 0x0055d3cc, 0x0055d3ea, 0x0055d3f1, // TMapDialog::Draw
		0x0055d41a, 0x0055d423, 0x0055d430,
		0x00562845, 0x00562866, 0x0056287d, 0x005628b4, // single-tile redraw (0x00562730)
		0x005628d0, 0x005628dc,
	};
	BOOL ok = TRUE;
	for (int i = 0; i < (int)(sizeof slotAccess / sizeof slotAccess[0]); i++) {
		DWORD disp = *(DWORD *)slotAccess[i];
		if (disp < CACHE_OLD_BASE || disp > CACHE_OLD_BASE + 6) {
			logf_("cache access %08lx holds displacement %lx: not relocating", (unsigned long)slotAccess[i], (unsigned long)disp);
			return FALSE; // nothing has been written yet, so the game is untouched
		}
	}
	for (int i = 0; i < (int)(sizeof slotAccess / sizeof slotAccess[0]); i++) {
		DWORD disp = *(DWORD *)slotAccess[i];
		ok &= patch_imm32(slotAccess[i], disp, disp - CACHE_OLD_BASE + CACHE_NEW_BASE);
	}

	// Slot counts.
	ok &= patch_imm32(0x0055cfb8, CACHE_OLD_SLOTS, CACHE_ALLOCATED_SLOTS); // FlushCache clears them all
	ok &= patch_imm32(0x0055d211, CACHE_OLD_SLOTS, CACHE_NEW_SLOTS);       // Draw: eviction pass
	ok &= patch_imm8(0x0055d3bf, CACHE_OLD_SLOTS, CACHE_NEW_SLOTS);        // Draw: free-slot search
	ok &= patch_imm8(0x0055d3dc, CACHE_OLD_SLOTS, CACHE_NEW_SLOTS);
	ok &= patch_imm8(0x0056284f, CACHE_OLD_SLOTS, CACHE_NEW_SLOTS);        // single-tile redraw
	ok &= patch_imm8(0x00562855, CACHE_OLD_SLOTS, CACHE_NEW_SLOTS);
	ok &= patch_imm8(0x00562874, CACHE_OLD_SLOTS, CACHE_NEW_SLOTS);

	// Object allocations, enlarged to hold the relocated array.
	DWORD size = CACHE_NEW_BASE + CACHE_ALLOCATED_SLOTS * 8;
	ok &= patch_imm32(0x0044e9e9, 0x40c, size); // map screen builder: TMapDialog
	ok &= patch_imm32(0x005595a7, 0x40c, size); // TMapDialog::CreateObject
	ok &= patch_imm32(0x0045fe58, 0x41c, size); // startup screens builder: TCitySiteView
	ok &= patch_imm32(0x0055acf8, 0x41c, size); // TCitySiteView::CreateObject
	ok &= patch_imm32(0x0055b8f9, 0x418, size); // TMapEditView::CreateObject

	// The bitmap holding the cached tiles: one 64-pixel cell per slot.
	ok &= patch_imm32(0x00559932, CACHE_OLD_SLOTS * 64, CACHE_ALLOCATED_SLOTS * 64); // TMapDialog::DoPostCreate
	ok &= patch_imm32(0x0055ae44, CACHE_OLD_SLOTS * 64, CACHE_ALLOCATED_SLOTS * 64); // TCitySiteView::DoPostCreate
	ok &= patch_imm32(0x0055ba2f, CACHE_OLD_SLOTS * 64, CACHE_ALLOCATED_SLOTS * 64); // TMapEditView::DoPostCreate
	return ok;
}

// Fixed "the map is 576 pixels / 9 tiles wide" numbers in the map code.
static BOOL widen_map_constants(int extra)
{
	BOOL ok = patch_imm8(0x0055d22c, 9, MAP_COLUMNS_WIDE);          // Draw: columns kept in the cache
	ok &= patch_imm32(0x0055aaee, 576, 576 + extra);                // SetUpperLeft: area to invalidate
	ok &= patch_imm32(0x0055ab1a, 640, 640 + extra);                // SetUpperLeft: scroll area
	ok &= patch_imm32(0x0055db92, 576, 576 + extra);                // DrawTile: clip rectangle
	// TWorldView::DrawDecorativeBorder: the right-hand frame strip sits 58 pixels in from the
	// right edge. (Shared with the 576-wide city-site view, whose strip now falls outside it.)
	ok &= patch_imm32(0x0060a252, 576 - 58, 576 - 58 + extra);
	// ImmediateDrawTile compares a 16-bit register: "66 81 f9 40 02".
	WORD *limit = (WORD *)0x0055a756;
	DWORD old;
	if (*limit == 576) {
		VirtualProtect(limit, 2, PAGE_EXECUTE_READWRITE, &old);
		*limit = (WORD)(576 + extra);
		VirtualProtect(limit, 2, old, &old);
	} else {
		ok = FALSE;
	}
	return ok;
}

static BOOL patch_imm16(DWORD address, WORD expected, WORD value)
{
	WORD *site = (WORD *)address;
	DWORD old;
	if (*site != expected) {
		logf_("patch site %08lx holds %04x, expected %04x: skipped", (unsigned long)address, *site, expected);
		return FALSE;
	}
	VirtualProtect(site, 2, PAGE_EXECUTE_READWRITE, &old);
	*site = value;
	VirtualProtect(site, 2, old, &old);
	FlushInstructionCache(GetCurrentProcess(), site, 2);
	return TRUE;
}

// The zoomed-out overview (TOceanDialog, 'DOOG') draws 16-pixel tiles: 36 columns by 32 rows in a
// 576x512 view. Its drawing loop, right-edge test, scroll limit and the minimap's "you are here"
// rectangle all carry that column count.
static BOOL widen_overview_constants(int extra)
{
	int columns = extra / 16;
	BOOL ok = patch_imm8(0x005b7c0e, 36, (BYTE)(36 + columns));          // draw loop: last column index
	ok &= patch_imm32(0x005b62fd, 600, 600 + extra);                      // draw loop: right-edge test
	ok &= patch_imm16(0x005b952e, 108 - 36, (WORD)(108 - 36 - columns)); // scroll: furthest left column
	ok &= patch_imm32(0x005b9533, 108 - 36, 108 - 36 - columns);
	ok &= patch_imm32(0x0060e3bb, 36, 36 + columns);                      // ZoomOut: minimap rectangle width
	ok &= patch_imm32(0x0060e686, 36, 36 + columns);                      // minimap set-up, zoomed out
	ok &= patch_imm32(0x005b95e0, 575, 575 + extra);                      // scroll: area to repaint
	ok &= patch_imm8(0x005b9492, 36, (BYTE)(36 + columns));               // "is this tile on screen" test
	ok &= patch_imm8(0x005b96f1, (BYTE)-18, (BYTE)-(18 + columns / 2));   // CenterOn: half the columns
	ok &= patch_imm8(0x005b97c4, 18, (BYTE)(18 + columns / 2));           // GetCenterTile
	ok &= patch_imm32(0x0055a8da, 575, 575 + extra);                      // close-up CenterOn: area to repaint
	return ok;
}

// ---------------------------------------------------------------------------------------------
// HD artwork groundwork: observing the game's picture copies
//
// Pictures reach the frame through CDib::CopyPixels (0x00487790), which the rest of the game calls
// through a jump thunk at 0x00403125. A CDib keeps its pixel pointer at +0x0c and a pointer to its
// BITMAPINFOHEADER at +0x10.

typedef void (__attribute__((thiscall)) *CopyPixelsFn)(void *src, void *dst, int sx, int sy, int w,
	int h, int dx, int dy, DWORD transparent);

static void __attribute__((thiscall)) traced_copy_pixels(BYTE *src, BYTE *dst, int sx, int sy, int w,
	int h, int dx, int dy, DWORD transparent)
{
	// Objects are reused at the same address for different pictures, so a picture is identified
	// by its pixel pointer and the rectangle copied.
	static struct { BYTE *bits; int w, h, dx, dy; } seen[400];
	static int seenCount;
	BOOL known = FALSE;
	BYTE *bits = readable(src, 0x14) ? *(BYTE **)(src + 0x0c) : NULL;
	for (int i = 0; i < seenCount; i++) {
		known |= seen[i].bits == bits && seen[i].w == w && seen[i].h == h && seen[i].dx == dx && seen[i].dy == dy;
	}
	if (!known && seenCount < 400 && bits && readable(dst, 0x14)) {
		seen[seenCount].bits = bits; seen[seenCount].w = w; seen[seenCount].h = h;
		seen[seenCount].dx = dx; seen[seenCount].dy = dy;
		seenCount++;
		const BITMAPINFOHEADER *sh = *(const BITMAPINFOHEADER **)(src + 0x10);
		const BITMAPINFOHEADER *dh = *(const BITMAPINFOHEADER **)(dst + 0x10);
		typedef void *(__cdecl *QuickOffscreenDib)(void);
		BOOL toFrame = dst == (BYTE *)((QuickOffscreenDib)0x004a8540)();
		if (readable(sh, sizeof *sh) && readable(dh, sizeof *dh)) {
			logf_("copy: picture %ldx%ld %dbpp -> %s %ldx%ld %dbpp, %dx%d from %d,%d to %d,%d key %08lx",
				sh->biWidth, labs(sh->biHeight), sh->biBitCount, toFrame ? "frame" : "other buffer",
				dh->biWidth, labs(dh->biHeight), dh->biBitCount, w, h, sx, sy, dx, dy, (unsigned long)transparent);
		}
	}
	((CopyPixelsFn)0x00487790)(src, dst, sx, sy, w, h, dx, dy, transparent);
}

static void trace_picture_copies(void)
{
	const DWORD thunk = 0x00403125, target = 0x00487790;
	DWORD *rel = (DWORD *)(thunk + 1), old;
	if (*(BYTE *)thunk != 0xe9 || thunk + 5 + *rel != target) {
		logf_("picture-copy thunk not as expected: tracing off");
		return;
	}
	VirtualProtect(rel, 4, PAGE_EXECUTE_READWRITE, &old);
	*rel = (DWORD)(DWORD_PTR)traced_copy_pixels - (thunk + 5);
	VirtualProtect(rel, 4, old, &old);
	FlushInstructionCache(GetCurrentProcess(), rel, 4);
	logf_("tracing picture copies");
}

// ---------------------------------------------------------------------------------------------
// HD artwork
//
// The game keeps drawing its 1x frame exactly as before. Alongside it the DLL keeps a frame twice
// the size. Whenever the game copies a picture into its frame and an upscaled version of that
// picture exists, the same rectangle is copied at 2x into the big frame, and the 1x pixels the
// game just wrote are remembered. When the frame is shown, every pixel that still holds the
// remembered value is taken from the big frame; anything the game has drawn over since (text,
// the map, sprites without HD art) is pixel-doubled from the 1x frame instead.
//
// Pictures are matched to their upscaled files by content: the pixel data in memory is the same
// as in the .rsrc archive it was loaded from, so a hash of a few slices identifies it.

typedef struct { DWORD hash; int w, h, id; char archive[24]; } HdIndexEntry;
typedef struct { DWORD hash; int w, h; DWORD *pixels; } HdImage; // pixels NULL: known to be missing

static char g_hdAssets[MAX_PATH], g_gameDir[MAX_PATH];
static HdIndexEntry *g_hdIndex;
static int g_hdIndexCount;
static HdImage g_hdImages[512];
static int g_hdImageCount;
static size_t g_hdBytes;

static DWORD *g_hdFrame;        // top-down, 2*g_hdW by 2*GAME_H
static WORD *g_hdShadow;        // the 1x value each pixel had when its 2x block was last written
static BYTE *g_hdState;         // 0 unknown, 1 block holds HD art, 2 block holds doubled 1x pixel
static BYTE *g_hdDirty;         // scratch: pixels found changed during the current compose
static int g_hdW;

// GDI+ flat API, used only to read PNG files.
typedef struct { UINT32 version; void *debugCallback; BOOL noThread; BOOL noCodecs; } GpStartupInput;
typedef struct { UINT width, height; INT stride; INT format; void *scan0; UINT_PTR reserved; } GpBitmapData;
typedef struct { INT x, y, w, h; } GpRectI;
int WINAPI GdiplusStartup(ULONG_PTR *token, const GpStartupInput *input, void *output);
int WINAPI GdipCreateBitmapFromFile(const WCHAR *path, void **bitmap);
int WINAPI GdipGetImageWidth(void *image, UINT *width);
int WINAPI GdipGetImageHeight(void *image, UINT *height);
int WINAPI GdipBitmapLockBits(void *bitmap, const GpRectI *rect, UINT flags, INT format, GpBitmapData *data);
int WINAPI GdipBitmapUnlockBits(void *bitmap, GpBitmapData *data);
int WINAPI GdipDisposeImage(void *image);
#define GP_FORMAT_32BPP_RGB 0x00022009

static DWORD fnv(DWORD h, const BYTE *p, size_t n)
{
	while (n--) h = (h ^ *p++) * 16777619u;
	return h;
}

// Hash of three slices of a picture's pixel data: start, middle and end.
#define HD_SLICE 2048
static DWORD picture_hash(const BYTE *pixels, size_t size, int w, int h)
{
	DWORD hash = fnv(2166136261u, (const BYTE *)&w, sizeof w);
	hash = fnv(hash, (const BYTE *)&h, sizeof h);
	size_t n = size < HD_SLICE ? size : HD_SLICE;
	hash = fnv(hash, pixels, n);
	if (size > 3 * HD_SLICE) {
		hash = fnv(hash, pixels + (size / 2 & ~(size_t)1), n);
		hash = fnv(hash, pixels + size - n, n);
	}
	return hash;
}

static size_t pixel_bytes_16(int w, int h) { return (size_t)((w + 1) & ~1) * 2 * h; }

static void hd_index_archive(const char *path, const char *name)
{
	HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE) return;
	DWORD got, header[4];
	if (ReadFile(f, header, sizeof header, &got, NULL) && got == sizeof header && header[0] == 0x63727372) { // "rsrc"
		DWORD count = header[3];
		DWORD *dir = malloc(count * 16);
		if (dir && ReadFile(f, dir, count * 16, &got, NULL) && got == count * 16) {
			BYTE *buf = malloc(3 * HD_SLICE);
			for (DWORD i = 0; i < count; i++) {
				DWORD type = dir[i * 4], id = dir[i * 4 + 1], offset = dir[i * 4 + 2];
				BITMAPINFOHEADER bih;
				if (type != 0x50494354) continue; // 'PICT'
				SetFilePointer(f, offset, NULL, FILE_BEGIN);
				if (!ReadFile(f, &bih, sizeof bih, &got, NULL) || got != sizeof bih) continue;
				if (bih.biSize != 40 || bih.biBitCount != 16 || bih.biCompression != BI_RGB) continue;
				int w = bih.biWidth, h = abs(bih.biHeight);
				size_t size = pixel_bytes_16(w, h), n = size < HD_SLICE ? size : HD_SLICE;
				DWORD base = offset + 40 + bih.biClrUsed * 4;
				DWORD hash = fnv(2166136261u, (const BYTE *)&w, sizeof w);
				hash = fnv(hash, (const BYTE *)&h, sizeof h);
				SetFilePointer(f, base, NULL, FILE_BEGIN);
				if (!ReadFile(f, buf, (DWORD)n, &got, NULL) || got != n) continue;
				hash = fnv(hash, buf, n);
				if (size > 3 * HD_SLICE) {
					SetFilePointer(f, base + (DWORD)(size / 2 & ~(size_t)1), NULL, FILE_BEGIN);
					ReadFile(f, buf, (DWORD)n, &got, NULL);
					hash = fnv(hash, buf, n);
					SetFilePointer(f, base + (DWORD)(size - n), NULL, FILE_BEGIN);
					ReadFile(f, buf, (DWORD)n, &got, NULL);
					hash = fnv(hash, buf, n);
				}
				HdIndexEntry *e = &g_hdIndex[g_hdIndexCount++];
				e->hash = hash; e->w = w; e->h = h; e->id = (int)id;
				lstrcpynA(e->archive, name, sizeof e->archive);
				if (g_hdIndexCount >= 4096) break;
			}
			free(buf);
		}
		free(dir);
	}
	CloseHandle(f);
}

static void hd_build_index(void)
{
	char pattern[MAX_PATH];
	WIN32_FIND_DATAA fd;
	g_hdIndex = calloc(4096, sizeof *g_hdIndex);
	snprintf(pattern, sizeof pattern, "%sData\\*.rsrc", g_gameDir);
	HANDLE find = FindFirstFileA(pattern, &fd);
	if (find == INVALID_HANDLE_VALUE || !g_hdIndex) return;
	do {
		char path[MAX_PATH], name[64];
		snprintf(path, sizeof path, "%sData\\%s", g_gameDir, fd.cFileName);
		lstrcpynA(name, fd.cFileName, sizeof name);
		char *dot = strrchr(name, '.');
		if (dot) *dot = 0;
		if (g_hdIndexCount < 4096) hd_index_archive(path, name);
	} while (FindNextFileA(find, &fd));
	FindClose(find);
	logf_("HD: indexed %d full-colour pictures", g_hdIndexCount);
}

static DWORD *hd_load_png(const char *path, int w2, int h2)
{
	static ULONG_PTR token;
	if (!token) {
		GpStartupInput input = { 1, NULL, FALSE, FALSE };
		if (GdiplusStartup(&token, &input, NULL) != 0) return NULL;
	}
	WCHAR wide[MAX_PATH];
	MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
	void *bitmap = NULL;
	if (GdipCreateBitmapFromFile(wide, &bitmap) != 0 || !bitmap) return NULL;
	DWORD *pixels = NULL;
	UINT w = 0, h = 0;
	GdipGetImageWidth(bitmap, &w);
	GdipGetImageHeight(bitmap, &h);
	GpBitmapData data;
	GpRectI all = { 0, 0, (INT)w, (INT)h };
	if ((int)w == w2 && (int)h == h2 && GdipBitmapLockBits(bitmap, &all, 1, GP_FORMAT_32BPP_RGB, &data) == 0) {
		pixels = malloc((size_t)w * h * 4);
		if (pixels) {
			for (UINT y = 0; y < h; y++) memcpy(pixels + (size_t)y * w, (BYTE *)data.scan0 + (size_t)y * data.stride, (size_t)w * 4);
		}
		GdipBitmapUnlockBits(bitmap, &data);
	}
	GdipDisposeImage(bitmap);
	return pixels;
}

// The upscaled picture for a 16-bit picture in memory, or NULL.
static const HdImage *hd_find(const BYTE *bits, int w, int h)
{
	size_t size = pixel_bytes_16(w, h);
	if (!readable(bits, size)) return NULL;
	DWORD hash = picture_hash(bits, size, w, h);
	for (int i = 0; i < g_hdImageCount; i++) {
		if (g_hdImages[i].hash == hash && g_hdImages[i].w == w && g_hdImages[i].h == h) {
			return g_hdImages[i].pixels ? &g_hdImages[i] : NULL;
		}
	}
	if (g_hdImageCount >= (int)(sizeof g_hdImages / sizeof g_hdImages[0]) || g_hdBytes > 400u * 1024 * 1024) {
		for (int i = 0; i < g_hdImageCount; i++) free(g_hdImages[i].pixels);
		g_hdImageCount = 0;
		g_hdBytes = 0;
		memset(g_hdState, 0, (size_t)g_hdW * GAME_H); // blocks referring to freed art are simply redrawn
	}
	HdImage *img = &g_hdImages[g_hdImageCount++];
	img->hash = hash; img->w = w; img->h = h; img->pixels = NULL;
	for (int i = 0; i < g_hdIndexCount; i++) {
		if (g_hdIndex[i].hash != hash || g_hdIndex[i].w != w || g_hdIndex[i].h != h) continue;
		char path[MAX_PATH];
		snprintf(path, sizeof path, "%s\\%s\\%d.png", g_hdAssets, g_hdIndex[i].archive, g_hdIndex[i].id);
		img->pixels = hd_load_png(path, w * 2, h * 2);
		if (img->pixels) g_hdBytes += (size_t)w * h * 16;
		logf_("HD: %s\\%d %dx%d %s", g_hdIndex[i].archive, g_hdIndex[i].id, w, h, img->pixels ? "loaded" : "NOT FOUND on disk");
		break;
	}
	return img->pixels ? img : NULL;
}

static BOOL hd_ready(void)
{
	if (g_hdFrame && g_hdW == g_gameW) return TRUE;
	if (g_hdFrame) return FALSE; // the frame width never changes after start-up
	BITMAPINFO bi = { 0 };
	bi.bmiHeader.biSize = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth = g_gameW * 2;
	bi.bmiHeader.biHeight = -GAME_H * 2;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	HDC screen = GetDC(NULL);
	g_hdDC = CreateCompatibleDC(screen);
	HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&g_hdFrame, NULL, 0);
	ReleaseDC(NULL, screen);
	g_hdShadow = calloc((size_t)g_gameW * GAME_H, sizeof *g_hdShadow);
	g_hdState = calloc((size_t)g_gameW * GAME_H, 1);
	g_hdDirty = calloc((size_t)g_gameW * GAME_H, 1);
	if (!g_hdDC || !bmp || !g_hdShadow || !g_hdState || !g_hdDirty) { g_hdFrame = NULL; return FALSE; }
	SelectObject(g_hdDC, bmp);
	g_hdW = g_gameW;
	hd_build_index();
	return TRUE;
}

// The 1x frame the HD state describes: the pixel buffer the game last copied artwork into. The
// game has several offscreen buffers of the same size, so they are told apart by address.
static const WORD *g_hdSourceBits;

static void hd_track_frame(const WORD *bits)
{
	if (bits == g_hdSourceBits) return;
	g_hdSourceBits = bits;
	memset(g_hdState, 0, (size_t)g_hdW * GAME_H);
}
static void __attribute__((thiscall)) hd_copy_pixels(BYTE *src, BYTE *dst, int sx, int sy, int w, int h,
	int dx, int dy, DWORD transparent)
{
	((CopyPixelsFn)0x00487790)(src, dst, sx, sy, w, h, dx, dy, transparent);

	if (transparent != 0xffffffff || !readable(src, 0x14) || !readable(dst, 0x14) || !hd_ready()) return;
	const BITMAPINFOHEADER *sh = *(const BITMAPINFOHEADER **)(src + 0x10);
	const BITMAPINFOHEADER *dh = *(const BITMAPINFOHEADER **)(dst + 0x10);
	if (!readable(sh, sizeof *sh) || sh->biBitCount != 16 || !readable(dh, sizeof *dh)) return;
	// Only copies into a frame-sized 16-bit buffer are of interest.
	if (dh->biBitCount != 16 || dh->biWidth != g_hdW || abs(dh->biHeight) != GAME_H) return;
	int pw = sh->biWidth, ph = abs(sh->biHeight), stride = (dh->biWidth + 1) & ~1;
	BOOL bottomUp = dh->biHeight > 0;
	const WORD *frame = *(const WORD **)(dst + 0x0c);
	const HdImage *art = frame ? hd_find(*(const BYTE **)(src + 0x0c), pw, ph) : NULL;
	if (!art) return;
	hd_track_frame(frame);

	// The same clipping the game applies, then to both pictures' bounds.
	if (dx < 0) { sx -= dx; w += dx; dx = 0; }
	if (dy < 0) { sy -= dy; h += dy; dy = 0; }
	if (sx < 0) { dx -= sx; w += sx; sx = 0; }
	if (sy < 0) { dy -= sy; h += sy; sy = 0; }
	if (w > pw - sx) w = pw - sx;
	if (h > ph - sy) h = ph - sy;
	if (w > g_hdW - dx) w = g_hdW - dx;
	if (h > GAME_H - dy) h = GAME_H - dy;
	if (w <= 0 || h <= 0) return;

	for (int y = 0; y < h; y++) {
		const WORD *row = frame + (size_t)(bottomUp ? GAME_H - 1 - (dy + y) : dy + y) * stride + dx;
		size_t at = (size_t)(dy + y) * g_hdW + dx;
		memcpy(g_hdShadow + at, row, (size_t)w * 2);
		memset(g_hdState + at, 1, (size_t)w);
		for (int half = 0; half < 2; half++) {
			memcpy(g_hdFrame + (size_t)((dy + y) * 2 + half) * g_hdW * 2 + dx * 2,
				art->pixels + (size_t)((sy + y) * 2 + half) * pw * 2 + sx * 2, (size_t)w * 8);
		}
	}
}

// Brings the big frame up to date for a rectangle of the 1x frame before it is shown.
static BOOL hd_compose(HDC source, int x0, int y0, int x1, int y1)
{
	// The bitmap being shown tells us which buffer this is and how its rows are laid out.
	DIBSECTION ds;
	HGDIOBJ bmp = GetCurrentObject(source, OBJ_BITMAP);
	if (!hd_ready() || !bmp || GetObjectA(bmp, sizeof ds, &ds) != sizeof ds || !ds.dsBm.bmBits) return FALSE;
	if (ds.dsBm.bmBitsPixel != 16 || ds.dsBm.bmWidth != g_hdW || ds.dsBm.bmHeight != GAME_H) return FALSE;
	const WORD *frame = ds.dsBm.bmBits;
	int stride = ds.dsBm.bmWidthBytes / 2;
	BOOL bottomUp = ds.dsBmih.biHeight > 0;
	hd_track_frame(frame);
	x0 = max(x0, 0); y0 = max(y0, 0); x1 = min(x1, g_hdW); y1 = min(y1, GAME_H);
	if (x1 <= x0 || y1 <= y0) return TRUE;
#define FRAME_ROW(yy) (frame + (size_t)(bottomUp ? GAME_H - 1 - (yy) : (yy)) * stride)

	// Pass 1: find the pixels the game has changed since their 2x block was written.
	for (int y = y0; y < y1; y++) {
		const WORD *row = FRAME_ROW(y);
		size_t at = (size_t)y * g_hdW;
		for (int x = x0; x < x1; x++) {
			WORD p = row[x];
			if (g_hdState[at + x] && g_hdShadow[at + x] == p) continue;
			g_hdState[at + x] = 2;
			g_hdShadow[at + x] = p;
			g_hdDirty[at + x] = 1;
		}
	}

	// Pass 2: redraw the non-HD blocks that changed or touch a change. Each 1x pixel becomes four,
	// each a 9:3:3:1 mix of it and its three neighbours on that side, which smooths text and
	// sprites instead of doubling their pixels.
	int bx0 = max(x0 - 1, 0), by0 = max(y0 - 1, 0), bx1 = min(x1 + 1, g_hdW), by1 = min(y1 + 1, GAME_H);
	for (int y = by0; y < by1; y++) {
		const WORD *rows[3] = { FRAME_ROW(max(y - 1, 0)), FRAME_ROW(y), FRAME_ROW(min(y + 1, GAME_H - 1)) };
		size_t at = (size_t)y * g_hdW;
		DWORD *out = g_hdFrame + (size_t)y * 2 * g_hdW * 2;
		for (int x = bx0; x < bx1; x++) {
			if (g_hdState[at + x] != 2) continue;
			BOOL touched = FALSE;
			for (int ny = max(y - 1, 0); ny <= min(y + 1, GAME_H - 1) && !touched; ny++) {
				for (int nx = max(x - 1, 0); nx <= min(x + 1, g_hdW - 1); nx++) {
					if (g_hdDirty[(size_t)ny * g_hdW + nx]) { touched = TRUE; break; }
				}
			}
			if (!touched) continue;
			WORD p = rows[1][x];
			for (int sub = 0; sub < 4; sub++) {
				int nx = sub & 1 ? min(x + 1, g_hdW - 1) : max(x - 1, 0);
				const WORD *other = rows[sub & 2 ? 2 : 0];
				WORD ph = rows[1][nx], pv = other[x], pd = other[nx];
				DWORD c = 0;
				for (int shift = 0; shift <= 10; shift += 5) {
					DWORD v = 9 * ((p >> shift) & 31) + 3 * ((ph >> shift) & 31) + 3 * ((pv >> shift) & 31) + ((pd >> shift) & 31);
					v = (v * 255 + 248) / 496; // 0..496 (16 x 31) -> 0..255
					c |= v << (shift == 0 ? 0 : shift == 5 ? 8 : 16);
				}
				out[(sub & 2 ? g_hdW * 2 : 0) + x * 2 + (sub & 1)] = c;
			}
		}
	}
	for (int y = y0; y < y1; y++) memset(g_hdDirty + (size_t)y * g_hdW + x0, 0, (size_t)(x1 - x0));
#undef FRAME_ROW
	return TRUE;
}

static void hd_install(void)
{
	const DWORD thunk = 0x00403125, target = 0x00487790;
	DWORD *rel = (DWORD *)(thunk + 1), old;
	if (*(BYTE *)thunk != 0xe9 || thunk + 5 + *rel != target) {
		logf_("HD: picture-copy thunk not as expected: HD art off");
		return;
	}
	VirtualProtect(rel, 4, PAGE_EXECUTE_READWRITE, &old);
	*rel = (DWORD)(DWORD_PTR)hd_copy_pixels - (thunk + 5);
	VirtualProtect(rel, 4, old, &old);
	FlushInstructionCache(GetCurrentProcess(), rel, 4);
	g_hd = TRUE;
	logf_("HD art on, from %s", g_hdAssets);
}

// Wide mode: make the game's own idea of the screen wider than 800. Addresses are for the v1.03
// exe.
static void widen_screen(int width)
{
	BOOL ok = patch_imm32(0x004aadc4, BASE_W, width);  // GetMainDevice: emulated Mac screen rect
	ok &= patch_imm32(0x0049615e, BASE_W, width);      // CIncludeView::OnDraw: frame buffer size
	ok &= patch_imm32(0x004a2abf, BASE_W, width);      // TWindow::Center: centring against the screen
	if (ok) g_gameW = width;
	logf_("wide mode %s (%d wide)", ok ? "on" : "FAILED", width);
	if (!ok) return;

	g_wideMap = relocate_tile_cache() && widen_map_constants(width - BASE_W) &&
		widen_overview_constants(width - BASE_W);
	logf_("wide map %s", g_wideMap ? "enabled" : "NOT enabled: the map stays 576 wide");
}

// Globals the game fills in before WinMain; adjusted once its windows exist.
static void widen_globals(void)
{
	if (g_gameW <= BASE_W) return;
	int *screenW = (int *)0x0075bc88, *columns = (int *)0x0075c048, *rows = (int *)0x0075c0d0;
	logf_("globals before: screen width %d, map columns+1 %d, cached row span %d", *screenW,
		(short)*columns, (short)*rows);
	*screenW = g_gameW; // also sizes the map's composition buffer
	if (g_wideMap) {
		*columns = MAP_COLUMNS_WIDE + 1; // limits how far right the map can scroll
		*rows = 8;                       // rows kept in the cache: 9, down from 11
		short *minimapColumns = (short *)0x0076091c; // minimap rectangle width for the close-up map
		logf_("minimap close-up columns before: %d", *minimapColumns);
		if (*minimapColumns == 10) *minimapColumns = MAP_COLUMNS_WIDE + 1; // stored as columns + 1
	}
}

#define HOOK(dll, name) hook_import(dll, #name, h_##name, &o_##name)

static void install(HMODULE self)
{
	char ini[MAX_PATH];
	GetModuleFileNameA(self, ini, sizeof ini);
	char *slash = strrchr(ini, '\\');
	strcpy(slash ? slash + 1 : ini, "imp2hd.ini");
	strcpy(g_logPath, ini);
	strcpy(strrchr(g_logPath, '.'), ".log");
	DeleteFileA(g_logPath);

	if (!GetPrivateProfileIntA("Display", "Enabled", 1, ini)) {
		logf_("disabled in imp2hd.ini");
		return;
	}
	g_smooth = GetPrivateProfileIntA("Display", "Smooth", 1, ini);
	g_integerScale = GetPrivateProfileIntA("Display", "IntegerScale", 0, ini);
	g_dumpViews = GetPrivateProfileIntA("Debug", "DumpViews", 0, ini);
	g_wideSteps = GetPrivateProfileIntA("Debug", "WideSteps", 7, ini);

	// Without this Windows scales the whole window itself on high-DPI displays, blurring it.
	SetProcessDPIAware();

	BOOL ok = HOOK("USER32.dll", CreateWindowExA);
	ok &= HOOK("USER32.dll", BeginPaint);
	ok &= HOOK("USER32.dll", GetDC);
	ok &= HOOK("USER32.dll", GetClientRect);
	ok &= HOOK("GDI32.dll", BitBlt);
	HOOK("USER32.dll", ScreenToClient);
	HOOK("USER32.dll", ClientToScreen);
	HOOK("USER32.dll", InvalidateRect);
	HOOK("USER32.dll", ValidateRect);
	HOOK("USER32.dll", RedrawWindow);
	HOOK("USER32.dll", SetCursor);
	HOOK("GDI32.dll", StretchBlt);
	HOOK("GDI32.dll", GetClipBox);
	HOOK("GDI32.dll", StretchDIBits);
	BOOL native = skip_mode_switch();
	lstrcpynA(g_gameDir, ini, sizeof g_gameDir);
	char *lastSlash = strrchr(g_gameDir, '\\');
	if (lastSlash) lastSlash[1] = 0;
	if (g_knownBuild && GetPrivateProfileIntA("Debug", "TraceCopies", 0, ini)) {
		trace_picture_copies();
	} else if (g_knownBuild && GetPrivateProfileIntA("Display", "HD", 0, ini)) {
		char fallback[MAX_PATH];
		snprintf(fallback, sizeof fallback, "%sdecomp\\assets\\x2", g_gameDir);
		GetPrivateProfileStringA("Display", "HDAssets", fallback, g_hdAssets, sizeof g_hdAssets, ini);
		hd_install();
	}
	// Four extra 64-pixel map columns: 1056x600, close to 16:9.
	if (native && GetPrivateProfileIntA("Display", "Wide", 0, ini)) widen_screen(BASE_W + 4 * 64);
	logf_("imp2hd loaded (smooth=%d integer=%d) core hooks %s, native resolution %s", g_smooth,
		g_integerScale, ok ? "ok" : "INCOMPLETE", native ? "kept" : "NOT kept");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(inst);
		load_real_ddraw();
		install(inst);
	}
	return TRUE;
}
