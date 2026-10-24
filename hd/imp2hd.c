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
#include <limits.h>
#include "layouts.h"

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
static int g_marginArt = 3; // beside centred screens: 0 black, 1 blurred edge, 2 tiled texture, 3 extended background
static BOOL g_screenOnTop, g_mapOnTop;
static BYTE *g_topScreen; // the screen on top, as last seen by watch_screens
static BOOL g_hdNative = TRUE; // full-screen paintings shown at the display's resolution (HDNative)
static char g_gameDir[MAX_PATH];
static BOOL g_twoStage; // smooth scaling through the 2x frame instead of GDI's slow HALFTONE
static HDC g_hdDC;
static BOOL hd_compose(HDC source, int x0, int y0, int x1, int y1);
static BOOL quad_present(HDC dst, HDC src, int x0, int y0, int x1, int y1, int width, DWORD rop);
static void hd_note_gdi_draw(HDC dst, int x, int y, int w, int h);
static BOOL hd_transfer_map(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop);
static BOOL hd_map_to_window(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop);
static void hd_forget_art(void);
static void hd_on_exit(void);
static BOOL hd_small_to_window(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop);
static void __attribute__((thiscall)) hd_copy_pixels(BYTE *src, BYTE *dst, int sx, int sy, int w, int h,
	int dx, int dy, DWORD transparent);
static WNDPROC g_frameProc, g_viewProc;
static int g_viewW = BASE_W, g_viewH = GAME_H; // current size of the view window, in pixels
static BOOL g_smooth = TRUE, g_integerScale = FALSE;
static char g_logPath[MAX_PATH];

// The log stays open: opening and closing it for each line cost 1-2 ms a time (the file is
// scanned on every close), which added up while the game ran. Each line is flushed, so the log
// is complete if the game crashes.
static void logf_(const char *fmt, ...)
{
	static FILE *f;
	if (!f) f = fopen(g_logPath, "a");
	if (!f) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fflush(f);
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
// gameWidth: the pop-up's width in game pixels, or 0 if not known.
static void popup_rect_to_frame(int *x, int *y, int *cx, int *cy, int gameWidth)
{
	RECT v;
	view_placement(&v);
	// Most pop-ups are placed for an 800-wide layout and are shifted by half the extra width. Some
	// are centred by TWindow::Center, which wide mode already makes centre on the full width;
	// those are left where they are.
	int shift = (g_gameW - BASE_W) / 2;
	if (x && gameWidth > 0 && abs((*x - v.left) * 2 + gameWidth - g_gameW) <= 4) shift = 0;
	if (x) *x = v.left + to_view_x(*x - v.left + shift);
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
			int gameWidth = 0;
			if (!(pos->flags & SWP_NOSIZE)) gameWidth = pos->cx;
			else { RECT r; if (GetWindowRect(hwnd, &r)) gameWidth = to_game_x(r.right - r.left); }
			if (!(pos->flags & SWP_NOMOVE)) popup_rect_to_frame(&pos->x, &pos->y, NULL, NULL, gameWidth);
			if (!(pos->flags & SWP_NOSIZE)) popup_rect_to_frame(NULL, NULL, &pos->cx, &pos->cy, 0);
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
static BOOL g_timing;
static LONGLONG g_presentTicks;
static int g_presentPixels;
static LONGLONG g_copyTicks, g_composeTicks, g_origCopyTicks, g_findTicks;
static int g_copyCalls;
static int g_traceBudget; // diagnostic: number of paint calls still to be logged
static int g_drawTrace; // diagnostic: direct-to-window drawing calls still to be logged
static int g_moveTraces; // diagnostic: mouse moves traced on the current screen
static BOOL g_wideMap; // tile cache relocated, so the map view itself may be widened
#define DIPLOMACY_MAP_SHIFT 125 // must match the diplomacy layouts' chart band
static BOOL g_diplomacyMapMoved; // the map of nations is drawn DIPLOMACY_MAP_SHIFT further right

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

// The battle screen's background: a stone title bar, thin pillars either side of the battlefield
// and the toolbar's panel along the bottom (the toolbar itself draws nothing but its buttons).
#define TAC_HOLE_X 12
#define TAC_HOLE_Y 29
#define TAC_HOLE_W 775
#define TAC_HOLE_H 480
#define TAC_TITLE_SPLIT 400    // the title bar is plain stone from about x=220 to x=560
#define TAC_TITLE_SLICE 300
#define TAC_PANEL_SPLIT1 212   // plain panel between the 'done' button and the unit description
#define TAC_PANEL_SPLIT2 404   // and between the description and the 'retreat' button
#define TAC_PANEL_SLICE 160    // two repeats of the panel's bottom ornament, 26 pixels each
#define TAC_SLICE_W 52

// One horizontal run of the widened background: destination columns [dstStart, dstEnd) come from
// the picture starting at srcStart. With tile > 0 the source is a strip that wide, repeated.
typedef struct { int dstStart, dstEnd, srcStart, tile; } BgSpan;

// A widened screen background. The rectangle of the view it frames (the map or the battlefield)
// is never painted: the game does not redraw that view after the background, it relies on the
// background leaving that area alone. Rows above and beside it are built from the upper spans,
// rows below it from the lower spans.
typedef struct {
	RECT hole;
	BgSpan upper[5], lower[5];
	int upperCount, lowerCount;
} BgLayout;

enum { BG_MAP, BG_BATTLE, BG_KINDS };
static BgLayout g_bgLayouts[BG_KINDS];
static void *g_backgroundVtables[BG_KINDS][0x101];

static void build_bg_layouts(void)
{
	int extra = g_gameW - BASE_W, half = extra / 2;
	// Map: everything from BG_SPLIT on moves right with the panel. The toolbar ribbon in the
	// bottom border is moved half the extra width so it stays centred under the map, with plain
	// border either side of it.
	BgLayout map = {
		{ MAP_HOLE_X, MAP_HOLE_Y, MAP_HOLE_X + MAP_HOLE_W + extra, MAP_HOLE_Y + MAP_HOLE_H },
		{
			{ 0, BG_SPLIT, 0, 0 },
			{ BG_SPLIT, BG_SPLIT + extra, BG_SLICE, BG_SLICE_W },
			{ BG_SPLIT + extra, g_gameW, BG_SPLIT, 0 },
		},
		{
			{ 0, RIBBON_LEFT, 0, 0 },
			{ RIBBON_LEFT, RIBBON_LEFT + half, RIBBON_RIGHT, BG_SPLIT - RIBBON_RIGHT },
			{ RIBBON_LEFT + half, RIBBON_RIGHT + half, RIBBON_LEFT, 0 },
			{ RIBBON_RIGHT + half, BG_SPLIT + extra, RIBBON_RIGHT, BG_SPLIT - RIBBON_RIGHT },
			{ BG_SPLIT + extra, g_gameW, BG_SPLIT, 0 },
		},
		3, 5,
	};
	// Battle: the title bar is lengthened in its middle. The toolbar panel is lengthened in two
	// places, half the extra width each, and its buttons are spread out to match (see
	// widen_battle_screen).
	BgLayout battle = {
		{ TAC_HOLE_X, TAC_HOLE_Y, TAC_HOLE_X + TAC_HOLE_W + extra, TAC_HOLE_Y + TAC_HOLE_H },
		{
			{ 0, TAC_TITLE_SPLIT, 0, 0 },
			{ TAC_TITLE_SPLIT, TAC_TITLE_SPLIT + extra, TAC_TITLE_SLICE, TAC_SLICE_W },
			{ TAC_TITLE_SPLIT + extra, g_gameW, TAC_TITLE_SPLIT, 0 },
		},
		{
			{ 0, TAC_PANEL_SPLIT1, 0, 0 },
			{ TAC_PANEL_SPLIT1, TAC_PANEL_SPLIT1 + half, TAC_PANEL_SLICE, TAC_SLICE_W },
			{ TAC_PANEL_SPLIT1 + half, TAC_PANEL_SPLIT2 + half, TAC_PANEL_SPLIT1, 0 },
			{ TAC_PANEL_SPLIT2 + half, TAC_PANEL_SPLIT2 + extra, TAC_PANEL_SLICE, TAC_SLICE_W },
			{ TAC_PANEL_SPLIT2 + extra, g_gameW, TAC_PANEL_SPLIT2, 0 },
		},
		3, 5,
	};
	g_bgLayouts[BG_MAP] = map;
	g_bgLayouts[BG_BATTLE] = battle;
}

static void __attribute__((thiscall)) wide_background_draw(BYTE *self, const int *area)
{
	typedef void (__attribute__((thiscall)) *CopyPixels)(void *src, void *dst, int sx, int sy, int w, int h, int dx, int dy, DWORD transparent);
	typedef void *(__cdecl *QuickOffscreenDib)(void);
	void *src = *(void **)(self + 0x90); // TPicture: the loaded picture
	void *dst = ((QuickOffscreenDib)0x004a8540)();
	if (!src || !dst) return;

	const BgLayout *layout = &g_bgLayouts[*(void ***)self == &g_backgroundVtables[BG_BATTLE][1] ? BG_BATTLE : BG_MAP];
	const int holeLeft = layout->hole.left, holeRight = layout->hole.right;
	const int bands[3][2] = { { 0, layout->hole.top }, { layout->hole.top, layout->hole.bottom }, { layout->hole.bottom, GAME_H } };

	for (int band = 0; band < 3; band++) {
		int top = max(area[1], bands[band][0]), bottom = min(area[3], bands[band][1]);
		if (bottom <= top) continue;
		const BgSpan *spans = band == 2 ? layout->lower : layout->upper;
		int count = band == 2 ? layout->lowerCount : layout->upperCount;

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
// (One copy per kind of screen; the copy also tells wide_background_draw which layout to use.)
static BOOL hook_background_draw(BYTE *picture, int kind)
{
	const DWORD originalDraw = 0x005c8800; // TOffLimitsPicture::Draw
	void **vtable = *(void ***)picture, **copy = g_backgroundVtables[kind];
	if (vtable == &copy[1]) return TRUE;
	if (!readable(vtable - 1, sizeof g_backgroundVtables[kind])) return FALSE;

	for (int slot = 0; slot < 0x100; slot++) {
		const BYTE *fn = (const BYTE *)vtable[slot];
		// Incremental-link builds reach functions through a "jmp" thunk.
		if (readable(fn, 5) && fn[0] == 0xe9) fn += 5 + *(const int *)(fn + 1);
		if ((DWORD)(DWORD_PTR)fn != originalDraw) continue;
		memcpy(copy, vtable - 1, sizeof g_backgroundVtables[kind]);
		copy[slot + 1] = (void *)wide_background_draw;
		*(void ***)picture = &copy[1];
		logf_("background draw replaced (vtable slot %d)", slot);
		return TRUE;
	}
	logf_("background draw of %s not found", view_class(picture));
	return FALSE;
}

// The background picture leaves a hole where the view it frames goes (TOffLimitsPicture); the
// hole was cut for the old size when the screen was built.
static void cut_background_hole(BYTE *picture, BYTE *view)
{
	typedef void **(__cdecl *NewRgnFn)(void);
	typedef void (__cdecl *SetRectRgnFn)(void **, short, short, short, short);
	typedef void (__attribute__((thiscall)) *SetRgnFn)(void *, void **);
	typedef void **(__cdecl *DisposeRgnFn)(void **);
	void **rgn = ((NewRgnFn)0x004a94e0)();
	((SetRectRgnFn)0x004ac6c0)(rgn, (short)VIEW_INT(view, V_X), (short)VIEW_INT(view, V_Y),
		(short)(VIEW_INT(view, V_X) + VIEW_INT(view, V_W)), (short)(VIEW_INT(view, V_Y) + VIEW_INT(view, V_H)));
	((SetRgnFn)0x005c88b0)(picture, rgn);
	((DisposeRgnFn)0x004a92d0)(rgn);
}

static void widen_parents(BYTE *picture)
{
	for (BYTE *v = picture; readable(v, 0x54); v = *(BYTE **)(v + V_PARENT)) {
		if (VIEW_INT(v, V_W) == BASE_W) VIEW_INT(v, V_W) = g_gameW;
	}
}

static void shift_view(BYTE *view, int dx)
{
	if (!view) return;
	VIEW_INT(view, V_X) += dx;
	update_coordinates(view); // only walks down when the view's own position changed
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
		if (hook_background_draw(picture, BG_MAP)) widen_parents(picture);
		else widen_parents(*(BYTE **)(picture + V_PARENT));
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
		cut_background_hole(picture, map);
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

// The battle screen ('main', TTacMapUberPicture): a 775x480 battlefield ('DLOG', TTacArmyView)
// framed by the background, the toolbar ('tool') below it, and the place name and turn count
// ('locn', 'turn') in the title bar. The battlefield picture is about twice as wide as the view
// and scrolls; the view's own width sets how far it scrolls, so it can simply be made wider.
static BOOL g_wideBattle; // the decorative corners' fixed positions were moved for the wider field

static void widen_battle_screen(BYTE *picture)
{
	int extra = g_gameW - BASE_W, half = extra / 2;
	BYTE *field = child_view(picture, ID4('D', 'L', 'O', 'G'));
	BYTE *toolbar = child_view(picture, ID4('t', 'o', 'o', 'l'));
	if (extra <= 0 || !g_wideBattle || !field || !toolbar || VIEW_INT(picture, V_W) != BASE_W) return;
	// Left at 800 wide the screen is presented centred, as before.
	if (!hook_background_draw(picture, BG_BATTLE)) return;
	widen_parents(picture);

	VIEW_INT(field, V_W) += extra;
	cut_background_hole(picture, field);

	// The toolbar's buttons follow the two places its panel was lengthened.
	VIEW_INT(toolbar, V_W) += extra;
	static const DWORD middle[] = { ID4('s', 'e', 'l', 'd') };
	static const DWORD right[] = { ID4('r', 'e', 't', 'r'), ID4('a', 'u', 't', 'o'), ID4('l', 'o', 'c', 'k'),
		ID4('c', 'u', 'r', 's'), ID4('e', 'n', 'd', 't') };
	for (int i = 0; i < (int)(sizeof middle / sizeof middle[0]); i++) shift_view(child_view(toolbar, middle[i]), half);
	for (int i = 0; i < (int)(sizeof right / sizeof right[0]); i++) shift_view(child_view(toolbar, right[i]), extra);
	shift_view(child_view(picture, ID4('t', 'u', 'r', 'n')), extra);

	InvalidateRect(g_view, NULL, FALSE);
	logf_("battle screen widened by %d", extra);
}

// ---------------------------------------------------------------------------------------------
// Wide mode: screens with a layout in layouts.h
//
// Their backgrounds are TBackgroundPictures, and some of their views are pictures too (a greyed-out
// row, for one). TPicture::Draw, which draws all of them, is redirected here: a picture that has
// been widened is drawn column by column from its layout, anything else as before.

#define MAX_WIDE_PICTURES 96
typedef struct {
	BYTE *picture, *screen;     // the picture and the screen it belongs to
	ScreenLayout layout;        // places its views
	ScreenLayout look;          // fills it: the layout or its variant for the artwork shown
	short lookPicture;          // the artwork `look` was chosen for
	short map[LAYOUT_MAX_BANDS][LAYOUT_MAX_W]; // from `look`
} WidePicture;
static WidePicture g_widePictures[MAX_WIDE_PICTURES];
static BOOL g_layoutDrawHooked;
static BYTE *g_pictureDrawRest; // TPicture::Draw's first instructions, then a jump to the rest

static WidePicture *wide_picture(const BYTE *picture)
{
	for (int i = 0; i < MAX_WIDE_PICTURES; i++)
		if (g_widePictures[i].picture && g_widePictures[i].picture == picture) return &g_widePictures[i];
	return NULL;
}

static void layout_stretch_fill(BYTE *src, BYTE *dst, int sx, int sw, int top, int bottom, int dx, int dy,
	int dw, int clipLeft, int clipRight);
static DWORD *hd_load_png(const char *path, int w2, int h2);
static BOOL hd_ready(void);
static void layout_patch_fill(BYTE *src, BYTE *dst, const LayoutInsert *in, int bandTop, int top, int bottom, int dx, int dy,
	int clipLeft, int clipRight);

#define GENERATED_BLEND 48 // columns of crossfade into the original; must match tools\outpaint.ps1
// Painted-out screens (layouts.h, 'G'): the 1x picture, and its 2x upscale when there is one.
typedef struct { short id; BOOL tried; DWORD *px1, *px2, *px4; } GeneratedArt;
static GeneratedArt g_generated[8];
static char g_gameDir[MAX_PATH];

static const GeneratedArt *generated_art(short id)
{
	if (!id) return NULL;
	GeneratedArt *slot = NULL;
	for (int i = 0; i < 8; i++) {
		if (g_generated[i].id == id) return g_generated[i].px1 ? &g_generated[i] : NULL;
		if (!slot && !g_generated[i].id) slot = &g_generated[i];
	}
	if (!slot) return NULL;
	slot->id = id;
	char path[MAX_PATH];
	snprintf(path, sizeof path, "%sdecomp\\assets\\wide\\%d.png", g_gameDir, id);
	slot->px1 = hd_load_png(path, g_gameW, GAME_H);
	snprintf(path, sizeof path, "%sdecomp\\assets\\x2\\_wide\\%d.png", g_gameDir, id);
	if (slot->px1) slot->px2 = hd_load_png(path, g_gameW * 2, GAME_H * 2);
	snprintf(path, sizeof path, "%sdecomp\\assets\\x4\\_wide\\%d.png", g_gameDir, id);
	if (slot->px1 && g_hd && g_hdNative) slot->px4 = hd_load_png(path, g_gameW * 4, GAME_H * 4);
	logf_("painted-out picture %d: %s, 2x %s, 4x %s", id, slot->px1 ? "loaded" : "missing",
		slot->px2 ? "loaded" : "missing", slot->px4 ? "loaded" : "missing");
	return slot->px1 ? slot : NULL;
}

static void generated_fill(short id, BYTE *dst, int x, int w, int top, int bottom, int ox, int oy);

// Picks the fill for the artwork a picture shows now: a variant entry for that artwork if there
// is one, or else the picture's own layout.
static void choose_look(WidePicture *wide, short artwork)
{
	const ScreenLayout *base = &wide->layout, *look = base;
	for (int i = 0; i < (int)(sizeof g_screenLayouts / sizeof g_screenLayouts[0]); i++) {
		const ScreenLayout *v = &g_screenLayouts[i];
		// (Matched by artwork, not class: screens sharing artwork share its fill.)
		if (v->picture && v->picture == artwork && v->id == base->id &&
			v->width == base->width && v->height == base->height && layout_extra(v) == layout_extra(base) &&
			layout_valid(v)) look = v;
	}
	wide->look = *look;
	if (wide->look.generated && !generated_art(wide->look.generated)) wide->look.generated = 0; // fall back
	wide->lookPicture = artwork;
	for (int b = 0; b < LAYOUT_MAX_BANDS && look->bands[b].bottom; b++)
		layout_band_map(&wide->look, &wide->look.bands[b], wide->map[b]);
}

static void __attribute__((thiscall)) layout_background_draw(BYTE *self, const int *area)
{
	typedef void (__attribute__((thiscall)) *DrawFn)(void *, const int *);
	typedef void (__attribute__((thiscall)) *CopyPixels)(void *src, void *dst, int sx, int sy, int w, int h, int dx, int dy, DWORD transparent);
	typedef void *(__cdecl *QuickOffscreenDib)(void);
	WidePicture *wide = wide_picture(self);
	// (A view that has since changed size, or another one at the same address, is drawn as before.)
	if (wide && (VIEW_INT(self, V_W) != wide->layout.width + layout_extra(&wide->layout) ||
		VIEW_INT(self, V_H) != wide->layout.height)) wide = NULL;
	void *src = *(void **)(self + 0x90), *dst = wide ? ((QuickOffscreenDib)0x004a8540)() : NULL;
	if (!wide || !src || !dst) {
		((DrawFn)g_pictureDrawRest)(self, area); // TPicture::Draw as it was
		return;
	}
	short artwork = *(short *)(self + 0x88);
	if (wide->layout.id && artwork != wide->lookPicture) choose_look(wide, artwork);
	CopyPixels copy = g_hd ? (CopyPixels)hd_copy_pixels : (CopyPixels)0x00487790;
	const ScreenLayout *s = &wide->look;
	int originX = VIEW_INT(self, 0x2c), originY = VIEW_INT(self, 0x30);
	int width = s->width + layout_extra(s);
	for (int b = 0; b < LAYOUT_MAX_BANDS && s->bands[b].bottom; b++) {
		int top = max(area[1], s->bands[b].top), bottom = min(area[3], s->bands[b].bottom);
		if (bottom <= top) continue;
		const short *map = wide->map[b];
		// Runs of columns that come from consecutive source columns are copied in one go.
		int left = max(area[0], 0), right = min(area[2], width);
		for (int x = left; x < right;) {
			int run = 1;
			if (map[x] < 0) {
				// Generated columns, from the painted-out picture; patches are drawn below.
				while (x + run < right && map[x + run] == map[x]) run++;
				if (map[x] == -1) generated_fill(s->generated, dst, x, run, top, bottom, originX, originY);
				x += run;
				continue;
			}
			while (x + run < right && map[x + run] == map[x] + run) run++;
			copy(src, dst, map[x], top, run, bottom - top, originX + x, originY + top, 0xffffffff);
			x += run;
		}
		// Beside each painted-out stretch, the painted-out picture also covers a band of the
		// original: there it holds a crossfade from the original into the generated part
		// (tools\outpaint.ps1), which hides the join.
		if (s->generated) {
			int before = 0;
			for (int i = 0; i < LAYOUT_MAX_INSERTS && s->bands[b].ins[i].width; i++) {
				const LayoutInsert *in = &s->bands[b].ins[i];
				if (in->mode == 'G') {
					int start = in->at + before, end = start + in->width;
					int bands[2][2] = { { start - GENERATED_BLEND, start }, { end, end + GENERATED_BLEND } };
					for (int k = 0; k < 2; k++) {
						int l = max(max(bands[k][0], left), 0), r = min(bands[k][1], right);
						if (r > l) generated_fill(s->generated, dst, l, r - l, top, bottom, originX, originY);
					}
				}
				before += in->width;
			}
		}
		// Stretched columns were copied as repeats of the nearest source column; they are redrawn
		// blended between neighbours.
		int before = 0;
		for (int i = 0; i < LAYOUT_MAX_INSERTS && s->bands[b].ins[i].width; i++) {
			const LayoutInsert *in = &s->bands[b].ins[i];
			if (in->mode == 'Q') {
				int start = in->at + before;
				layout_patch_fill(src, dst, in, s->bands[b].top, top, bottom, originX + start, originY,
					originX + max(left, start), originX + min(right, start + in->width));
			}
			if (in->mode == 'S') {
				int from = in->src + before, total = in->srcW + in->width;
				layout_stretch_fill(src, dst, in->src, in->srcW, top, bottom, originX + from, originY,
					total, originX + max(left, from), originX + min(right, from + total));
			}
			before += in->width;
		}
	}
}

// TPicture::Draw starts "sub esp,30h; push ebx; push ebp", five bytes that become a jump here. They
// are copied, followed by a jump back past them, to make the original callable.
static void hook_layout_draw(void)
{
	BYTE *site = (BYTE *)0x004a3e20;
	static const BYTE expected[] = { 0x83, 0xec, 0x30, 0x53, 0x55 };
	if (g_layoutDrawHooked) return;
	if (memcmp(site, expected, sizeof expected)) {
		logf_("TPicture::Draw not as expected: wide screen layouts off");
		return;
	}
	g_pictureDrawRest = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!g_pictureDrawRest) return;
	memcpy(g_pictureDrawRest, expected, sizeof expected);
	g_pictureDrawRest[5] = 0xe9;
	*(int *)(g_pictureDrawRest + 6) = (int)((site + 5) - (g_pictureDrawRest + 10));
	DWORD old;
	VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old);
	site[0] = 0xe9;
	*(int *)(site + 1) = (int)((BYTE *)layout_background_draw - (site + 5));
	VirtualProtect(site, 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), site, 5);
	g_layoutDrawHooked = TRUE;
}

static const ScreenLayout *find_layout(BYTE *picture)
{
	for (int i = 0; i < (int)(sizeof g_screenLayouts / sizeof g_screenLayouts[0]); i++) {
		const ScreenLayout *s = &g_screenLayouts[i];
		if (s->picture) continue; // fill for one artwork only (see choose_look)
		if ((!strncmp(s->cls, "TDiplomacy", 10) || !strcmp(s->cls, "TBattleReportView")) && !g_diplomacyMapMoved) continue; // its map would not follow
		if (strcmp(view_class(picture), s->cls) || (DWORD)VIEW_INT(picture, V_ID) != s->id) continue;
		if (VIEW_INT(picture, V_W) != s->width || VIEW_INT(picture, V_H) != s->height) continue;
		if (s->child && !child_view(picture, s->child)) continue;
		return s;
	}
	return NULL;
}

static WidePicture *register_wide_picture(BYTE *picture, BYTE *screen, const ScreenLayout *layout)
{
	WidePicture *slot = wide_picture(picture);
	for (int i = 0; i < MAX_WIDE_PICTURES && !slot; i++) if (!g_widePictures[i].picture) slot = &g_widePictures[i];
	if (!slot) return NULL;
	slot->picture = picture;
	slot->screen = screen;
	slot->layout = *layout;
	slot->look = *layout;
	if (slot->look.generated && !generated_art(slot->look.generated)) slot->look.generated = 0; // fall back
	slot->lookPicture = -1;
	for (int b = 0; b < LAYOUT_MAX_BANDS && layout->bands[b].bottom; b++)
		layout_band_map(&slot->look, &slot->look.bands[b], slot->map[b]);
	return slot;
}

// Whether a view is drawn by TPicture::Draw: its virtual table leads there.
static BOOL drawn_as_picture(const BYTE *view)
{
	void **vtable = *(void ***)view;
	if (!readable(vtable, 0x100 * sizeof(void *))) return FALSE;
	for (int slot = 0; slot < 0x100; slot++) {
		const BYTE *fn = (const BYTE *)vtable[slot];
		if (readable(fn, 5) && fn[0] == 0xe9) fn += 5 + *(const int *)(fn + 1);
		DWORD target = (DWORD)(DWORD_PTR)fn;
		if (target == 0x004a3e20 || target == 0x005c7d60) return TRUE; // TPicture, TBackgroundPicture
	}
	return FALSE;
}

// The game keeps moving some views after a screen is built: tabs and panels are swapped by moving
// them between their place and a parking spot far off screen, and some views are created later.
// So the layout is not applied once but kept: every view's position as the game set it (in the
// 800-wide layout) is remembered, and whenever the game has changed it since, the view is placed
// again from the new value.
#define VIEW_RECORDS 4096
typedef struct { BYTE *view, *screen; int origX, origW, setX, setW; } ViewRecord;
static ViewRecord g_viewRecords[VIEW_RECORDS];

static ViewRecord *view_record(BYTE *view, BYTE *screen)
{
	unsigned h = ((unsigned)(DWORD_PTR)view >> 3) % VIEW_RECORDS;
	for (int probe = 0; probe < VIEW_RECORDS; probe++, h = (h + 1) % VIEW_RECORDS) {
		ViewRecord *r = &g_viewRecords[h];
		if (r->view == view) return r;
		if (!r->view) {
			r->view = view;
			r->screen = screen;
			r->origX = VIEW_INT(view, V_X);
			r->origW = VIEW_INT(view, V_W);
			r->setX = INT_MIN;
			return r;
		}
	}
	return NULL;
}

// The layout in force below a picture, and where that picture's left edge was and is.
typedef struct { const ScreenLayout *layout; int origLeft, newLeft, top; } LayoutContext;

static int sync_layout(BYTE *screen, BYTE *view, const LayoutContext *ctx, int parentOrigX, int parentNewX)
{
	int changed = 0;
	BYTE *list = *(BYTE **)(view + V_SUBVIEWS);
	if (!readable(list, 8)) return 0;
	for (void **node = *(void ***)(list + 4); readable(node, 12); node = (void **)node[0]) {
		BYTE *child = (BYTE *)node[2];
		if (!readable(child, 0x54)) continue;
		ViewRecord *rec = view_record(child, screen);
		if (!rec) return changed;
		if (rec->setX != INT_MIN && (VIEW_INT(child, V_X) != rec->setX || VIEW_INT(child, V_W) != rec->setW)) {
			if (g_dumpViews) {
				static int logged;
				DWORD id = (DWORD)VIEW_INT(child, V_ID);
				if (logged++ < 200) logf_("  game moved '%c%c%c%c' (%s): x %d -> %d, w %d -> %d (originally %d)", (char)(id >> 24),
					(char)(id >> 16), (char)(id >> 8), (char)id, view_class(child), rec->setX, VIEW_INT(child, V_X),
					rec->setW, VIEW_INT(child, V_W), rec->origX);
			}
			rec->origX = VIEW_INT(child, V_X); // moved or resized by the game since
			rec->origW = VIEW_INT(child, V_W);
		}
		int origX = parentOrigX + rec->origX, w = rec->origW;
		int y = VIEW_INT(child, 0x30) - ctx->top, h = VIEW_INT(child, V_H);
		int nx, nw;
		layout_place(ctx->layout, origX - ctx->origLeft, y, w, h, &nx, &nw);
		LayoutContext inner = *ctx;
		WidePicture *wide = wide_picture(child);
		if (nw != w && !wide) {
			const ScreenLayout *own = find_layout(child);
			ScreenLayout derived;
			if (own && own->width + layout_extra(own) == nw && layout_valid(own)) {
				// A picture with a layout of its own (identified while it still has its old size).
				wide = register_wide_picture(child, screen, own);
			} else if (drawn_as_picture(child)) {
				// A picture is lengthened the same way as the background under it; one that cannot
				// be (its fill would come from outside it) keeps its size and moves with its middle.
				if (!ctx->layout->fixedPictures && layout_derive(ctx->layout, origX - ctx->origLeft, y, w, h, &derived))
					wide = register_wide_picture(child, screen, &derived);
				if (!wide) {
					const LayoutBand *band = layout_band(ctx->layout, y + h / 2);
					nx = origX - ctx->origLeft + layout_shift_left(band, origX - ctx->origLeft + w / 2);
					nw = w;
				}
			}
		}
		if (wide && wide->layout.id) {
			// The views inside a picture with its own layout follow that layout, or its variant for
			// the artwork the picture shows now (the industry page has one per tab).
			short artwork = *(short *)(child + 0x88);
			if (artwork != wide->lookPicture) choose_look(wide, artwork);
			inner.layout = &wide->look;
			inner.origLeft = origX;
			inner.newLeft = ctx->newLeft + nx;
			inner.top = VIEW_INT(child, 0x30);
		}
		int newX = ctx->newLeft + nx;
		if (VIEW_INT(child, V_X) != newX - parentNewX || VIEW_INT(child, V_W) != nw) {
			VIEW_INT(child, V_X) = newX - parentNewX;
			VIEW_INT(child, V_W) = nw;
			update_coordinates(child);
			changed++;
		}
		rec->setX = newX - parentNewX;
		rec->setW = nw;
		changed += sync_layout(screen, child, &inner, origX, newX);
	}
	return changed;
}

static void sync_layout_screen(BYTE *screen)
{
	WidePicture *wide = wide_picture(screen);
	if (!wide) return;
	LayoutContext ctx = { &wide->layout, 0, 0, VIEW_INT(screen, 0x30) };
	if (sync_layout(screen, screen, &ctx, 0, 0)) {
		// Through the game (TView::InvalidateRect), so that the views themselves are redrawn and
		// not just the last frame shown again.
		typedef void (__attribute__((thiscall)) *Invalidate)(void *, const RECT *, int);
		RECT all = { 0, 0, VIEW_INT(screen, V_W), VIEW_INT(screen, V_H) };
		((Invalidate)0x0049fbf0)(screen, &all, 1);
	}
}

static BOOL widen_layout_screen(BYTE *screen)
{
	const ScreenLayout *s = find_layout(screen);
	if (!s || !g_layoutDrawHooked || g_gameW != BASE_W + LAYOUT_EXTRA) return FALSE;
	if (!layout_valid(s) || layout_extra(s) != LAYOUT_EXTRA || VIEW_INT(screen, 0x2c) != 0) {
		logf_("layout '%s' is not valid here", s->name);
		return FALSE;
	}
	if (!register_wide_picture(screen, screen, s)) return FALSE;
	widen_parents(screen);
	sync_layout_screen(screen);
	logf_("screen '%s' widened", s->name);
	return TRUE;
}

// Forgets the widened pictures and view records of screens that have gone (their memory may be
// reused for other views).
static void forget_layout_screens(BYTE **current, int count)
{
	BOOL any = FALSE;
	for (int i = 0; i < MAX_WIDE_PICTURES; i++) {
		if (!g_widePictures[i].picture) continue;
		BOOL alive = FALSE;
		for (int j = 0; j < count; j++) alive |= current[j] == g_widePictures[i].screen;
		if (!alive) {
			if (g_widePictures[i].picture == g_widePictures[i].screen) any = TRUE;
			g_widePictures[i].picture = g_widePictures[i].screen = NULL;
		}
	}
	if (!any) return;
	// Rebuilt without the records of the screens gone, so the probe chains stay intact.
	static ViewRecord keep[VIEW_RECORDS];
	int kept = 0;
	for (int i = 0; i < VIEW_RECORDS; i++) {
		if (!g_viewRecords[i].view) continue;
		for (int j = 0; j < MAX_WIDE_PICTURES; j++) {
			if (g_widePictures[j].screen == g_viewRecords[i].screen && g_widePictures[j].picture) {
				keep[kept++] = g_viewRecords[i];
				break;
			}
		}
	}
	memset(g_viewRecords, 0, sizeof g_viewRecords);
	for (int i = 0; i < kept; i++) {
		ViewRecord *r = view_record(keep[i].view, keep[i].screen);
		if (r) *r = keep[i];
	}
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
static BOOL is_battle_screen(const BYTE *screen) { return strcmp(view_class(screen), "TTacMapUberPicture") == 0; }
// Screens laid out for the full width; every other screen is presented centred.
static BOOL is_wide_screen(BYTE *screen)
{
	return is_map_screen(screen) || (is_battle_screen(screen) && VIEW_INT(screen, V_W) == g_gameW) ||
		wide_picture(screen) != NULL;
}


// The battlefield picture is drawn 29 columns wide (30 tiles and a half, for the odd rows) with
// the battle's own columns at its right-hand end; a battle of fewer columns starts further into
// the picture. A wide view on a small battle (fewer than about 20 columns) would reach past the
// picture's right edge, so the view is scrolled left instead, onto the picture's unused terrain
// beside the field. The game's scroll code never moves it back: it only scrolls a field wider
// than the view, and CenterOnTile's reset to 0 is undone here before the next paint.
static void keep_battlefield_in_picture(BYTE *screen)
{
	BYTE *field = child_view(screen, ID4('D', 'L', 'O', 'G'));
	BYTE *battle = field ? *(BYTE **)(field + 100) : NULL;
	if (!field || !readable(battle, 0x50)) return; // no battle loaded into the view yet
	int tile = VIEW_INT(field, 0x88), columns = VIEW_INT(battle, 0x48);
	short *scroll = (short *)(field + 0x78);
	static BYTE *logged;
	if (field != logged) {
		logged = field;
		logf_("battlefield: view %d wide, tile %d, %d columns, field %d wide, scroll %d", VIEW_INT(field, V_W),
			tile, columns, *(short *)(field + 0x7a), *scroll);
	}
	if (tile <= 0 || columns <= 0 || columns > 29) return;
	int offset = (29 - columns) * tile, pictureW = 30 * tile + tile / 2;
	int furthest = pictureW - VIEW_INT(field, V_W) - offset;
	if (*scroll > furthest && furthest >= -offset) {
		*scroll = (short)furthest;
		typedef void (__attribute__((thiscall)) *Invalidate)(void *, const RECT *, int);
		RECT all = { 0, 0, VIEW_INT(field, V_W), VIEW_INT(field, V_H) };
		((Invalidate)0x0049fbf0)(field, &all, 1); // TView::InvalidateRect, in the view's coordinates
	}
}

// Called before each paint and from a timer. The map screen is widened when it is built; every
// other screen was designed for 800 and is presented centred instead (see g_offsetX).
static void watch_screens_now(void);

// Diagnostic wrapper: how long the screen bookkeeping takes.
static void watch_screens(void)
{
	LARGE_INTEGER a, b, f;
	if (g_timing) QueryPerformanceCounter(&a);
	watch_screens_now();
	if (!g_timing) return;
	QueryPerformanceCounter(&b); QueryPerformanceFrequency(&f);
	double ms = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
	if (ms > 0.5) logf_("watch_screens %.2f ms", ms);
}

static void watch_screens_now(void)
{
	static BYTE *handled[32];
	static int handledCount;
	if (g_gameW <= BASE_W && !g_dumpViews) return;
	BYTE *base = screens_container();

	BYTE *current[32], *top = NULL;
	int count = 0;
	BYTE *list = base ? *(BYTE **)(base + V_SUBVIEWS) : NULL;
	// Records of screens that have gone are dropped before any new screen is laid out: its views
	// may be where the old screen's were.
	if (readable(list, 8)) {
		BYTE *present[32];
		int n = 0;
		for (void **node = *(void ***)(list + 4); readable(node, 12) && n < 32; node = (void **)node[0]) present[n++] = (BYTE *)node[2];
		forget_layout_screens(present, n);
	}
	if (readable(list, 8)) {
		for (void **node = *(void ***)(list + 4); readable(node, 12) && count < 32; node = (void **)node[0]) {
			BYTE *screen = (BYTE *)node[2];
			if (!readable(screen, 0x54)) continue;
			// Some screens attach a small view of their own here too (the trade screen's help
			// text); what is on top is the last full-size one.
			if (VIEW_INT(screen, V_W) >= BASE_W && VIEW_INT(screen, V_H) >= GAME_H) top = screen;

			BOOL seen = FALSE;
			for (int i = 0; i < handledCount; i++) seen |= handled[i] == screen;
			if (!seen) {
				BOOL map = is_map_screen(screen), battle = is_battle_screen(screen);
				// The map view is one of the last things attached; wait until the screen is built.
				if ((map || battle) && !child_view(screen, ID4('D', 'L', 'O', 'G'))) continue;
				if (battle && !child_view(screen, ID4('t', 'u', 'r', 'n'))) continue;

				if (g_gameW > BASE_W) {
					if (map) widen_map_screen(screen);
					else if (battle) widen_battle_screen(screen);
					else widen_layout_screen(screen);
				}
				if (g_dumpViews) { g_traceBudget = 60; g_moveTraces = 0; }
				DWORD id = (DWORD)VIEW_INT(screen, V_ID);
				logf_("screen %s '%c%c%c%c' at %d,%d size %dx%d", view_class(screen), (char)(id >> 24),
					(char)(id >> 16), (char)(id >> 8), (char)id, VIEW_INT(screen, V_X), VIEW_INT(screen, V_Y),
					VIEW_INT(screen, V_W), VIEW_INT(screen, V_H));
				if (g_dumpViews) {
					int budget = 3000;
					dump_view(screen, 1, &budget);
				}
			}
			current[count++] = screen;
		}
	}
	memcpy(handled, current, sizeof(BYTE *) * count);
	handledCount = count;
	forget_layout_screens(current, count);
	for (int i = 0; i < count; i++) sync_layout_screen(current[i]);
	// Whatever is on top decides how the frame is presented. Only the map and battle screens use the full width;
	// with no screen at all (the intro movies) the 800-wide layout is centred too.
	int offset = g_gameW > BASE_W && !(top && is_wide_screen(top)) ? (g_gameW - BASE_W) / 2 : 0;
	if (top && is_battle_screen(top)) keep_battlefield_in_picture(top);
	g_topScreen = top;
	g_screenOnTop = top != NULL;
	g_mapOnTop = top && is_map_screen(top);
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
	if (msg == WM_PAINT && g_timing) {
		// Diagnostic: how long the game takes to paint, and how much of that is presenting.
		LARGE_INTEGER a, b, freq;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&a);
		g_presentTicks = 0; g_presentPixels = 0; g_copyTicks = g_composeTicks = g_origCopyTicks = g_findTicks = 0; g_copyCalls = 0;
		LRESULT r = CallWindowProcA(g_viewProc, hwnd, msg, wp, lp);
		QueryPerformanceCounter(&b);
		double ms = (b.QuadPart - a.QuadPart) * 1000.0 / freq.QuadPart;
		double k = 1000.0 / freq.QuadPart;
		if (ms > 3) logf_("paint %.1f ms: presenting %.1f (compose %.1f), game copies %.1f in %d calls, HD copy work %.1f (matching %.1f); %d game pixels", ms,
			g_presentTicks * k, g_composeTicks * k, g_origCopyTicks * k, g_copyCalls, g_copyTicks * k, g_findTicks * k, g_presentPixels);
		return r;
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
			if (g_dumpViews && msg == WM_LBUTTONDOWN) g_traceBudget = 40;
			if (g_dumpViews && msg == WM_MOUSEMOVE && g_drawTrace < 12) {
				if (g_moveTraces++ < 40) { g_drawTrace = 12; logf_("move to game %d,%d", (short)LOWORD(lp), (short)HIWORD(lp)); }
			}
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
	if (msg == WM_NCDESTROY) {
		g_frame = NULL;
		hd_on_exit();
	}
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
		if (scaled()) popup_rect_to_frame(&x, &y, &w, &h, w);
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
	if (g_hd) hd_note_gdi_draw(dst, x, y, w, h);
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
	if (g_hd) hd_note_gdi_draw(dst, x, y, w, h);
	return o_StretchDIBits(dst, x, y, w, h, sx, sy, sw, sh, bits, info, usage, rop);
}

// Margin texture: the dark damask pattern from the map screen's side panel, read from the
// player's own Data\MapView.rsrc (picture 4122) and tiled. The pattern repeats exactly every
// 56x91 pixels. Built once as a 2x picture one margin wide; NULL if it cannot be loaded.
static HDC margin_texture(void)
{
	enum { PICTURE = 4122, CELL_X = 60, CELL_Y = 160, CELL_W = 56, CELL_H = 91 };
	static HDC dc;
	static BOOL tried;
	if (tried) return dc;
	tried = TRUE;

	int marginW = (g_gameW - BASE_W) / 2;
	WORD *cell = malloc(CELL_W * CELL_H * sizeof(WORD)), *flat = malloc((size_t)marginW * GAME_H * sizeof(WORD));
	char path[MAX_PATH];
	snprintf(path, sizeof path, "%sData\\MapView.rsrc", g_gameDir);
	HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	BOOL loaded = FALSE;
	DWORD got, header[4];
	if (cell && flat && marginW > 0 && f != INVALID_HANDLE_VALUE && ReadFile(f, header, sizeof header, &got, NULL) &&
		got == sizeof header && header[0] == 0x63727372 && header[3] < 4096) {
		DWORD *dir = malloc(header[3] * 16);
		if (dir && ReadFile(f, dir, header[3] * 16, &got, NULL) && got == header[3] * 16) {
			for (DWORD i = 0; i < header[3] && !loaded; i++) {
				BITMAPINFOHEADER bih;
				if (dir[i * 4] != 0x50494354 || dir[i * 4 + 1] != PICTURE) continue;
				SetFilePointer(f, dir[i * 4 + 2], NULL, FILE_BEGIN);
				if (!ReadFile(f, &bih, sizeof bih, &got, NULL) || got != sizeof bih) break;
				int w = bih.biWidth, h = abs(bih.biHeight), rowBytes = ((w + 1) & ~1) * 2;
				if (bih.biBitCount != 16 || bih.biCompression != BI_RGB || w < CELL_X + CELL_W || h < CELL_Y + CELL_H) break;
				loaded = TRUE;
				for (int y = 0; y < CELL_H && loaded; y++) {
					int row = bih.biHeight > 0 ? h - 1 - (CELL_Y + y) : CELL_Y + y;
					SetFilePointer(f, dir[i * 4 + 2] + 40 + bih.biClrUsed * 4 + row * rowBytes + CELL_X * 2, NULL, FILE_BEGIN);
					loaded = ReadFile(f, cell + y * CELL_W, CELL_W * 2, &got, NULL) && got == CELL_W * 2;
				}
			}
		}
		free(dir);
	}
	if (f != INVALID_HANDLE_VALUE) CloseHandle(f);

	DWORD *out = NULL;
	if (loaded) {
		BITMAPINFO bi = { 0 };
		bi.bmiHeader.biSize = sizeof bi.bmiHeader;
		bi.bmiHeader.biWidth = marginW * 2;
		bi.bmiHeader.biHeight = -GAME_H * 2;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		HDC screen = GetDC(NULL);
		dc = CreateCompatibleDC(screen);
		HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&out, NULL, 0);
		ReleaseDC(NULL, screen);
		if (dc && dib) SelectObject(dc, dib); else dc = NULL;
	}
	if (dc) {
		for (int y = 0; y < GAME_H; y++) {
			for (int x = 0; x < marginW; x++) flat[y * marginW + x] = cell[(y % CELL_H) * CELL_W + x % CELL_W];
		}
		// Doubled with the same 9:3:3:1 smoothing as the rest of the picture, slightly dimmed so
		// the screen in the middle stays the brightest thing.
		for (int y = 0; y < GAME_H; y++) {
			for (int x = 0; x < marginW; x++) {
				WORD p = flat[y * marginW + x];
				for (int sub = 0; sub < 4; sub++) {
					int nx = sub & 1 ? min(x + 1, marginW - 1) : max(x - 1, 0);
					int ny = sub & 2 ? min(y + 1, GAME_H - 1) : max(y - 1, 0);
					WORD ph = flat[y * marginW + nx], pv = flat[ny * marginW + x], pd = flat[ny * marginW + nx];
					DWORD c = 0;
					for (int shift = 0; shift <= 10; shift += 5) {
						DWORD v = 9 * ((p >> shift) & 31) + 3 * ((ph >> shift) & 31) + 3 * ((pv >> shift) & 31) + ((pd >> shift) & 31);
						c |= ((v >> 1) * 4 / 5) << (shift == 0 ? 0 : shift == 5 ? 8 : 16);
					}
					out[(size_t)(y * 2 + (sub >> 1)) * marginW * 2 + x * 2 + (sub & 1)] = c;
				}
			}
		}
	}
	logf_("margin texture %s", dc ? "loaded" : "not available: using the blurred fill");
	free(cell);
	free(flat);
	return dc;
}

static const WORD *dc_pixels(HDC dc, int *w, int *h, int *stride, BOOL *bottomUp);

// Margin extension: continues the background at each side of a centred screen by reflecting it
// about the screen's edge, shaded darker toward the outside.
//
// Each row is reflected only as far in as its background goes: the scan stops where something
// clearly brighter than the row's edge begins (a button, a scroll, a frame). A row whose
// background is at least a margin wide is reflected once with no repeat; a shorter one bounces
// back and forth within its own background. Things that reach the very edge (ribbon ends) show
// up as short runs of rows much brighter than the rows around them, and take their pixels from
// the background rows just below or above instead.
// Returns a DC holding both margins at 2x, left then right, or NULL.
static HDC margin_extension(HDC src)
{
	enum { EDGE = 24, WINDOW = 90, RUN_MAX = 110, SPAN = 6, ERODE = 8, SMOOTH = 4 };
	static HDC dc;
	static DWORD *out;
	static WORD *flat;
	static int marginW;
	int fw, fh, stride;
	BOOL bottomUp;
	const WORD *frame = dc_pixels(src, &fw, &fh, &stride, &bottomUp);
	if (!frame || fw < BASE_W || fh != GAME_H) return NULL;
	if (!dc) {
		marginW = (g_gameW - BASE_W) / 2;
		BITMAPINFO bi = { 0 };
		bi.bmiHeader.biSize = sizeof bi.bmiHeader;
		bi.bmiHeader.biWidth = marginW * 4;
		bi.bmiHeader.biHeight = -GAME_H * 2;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		HDC screen = GetDC(NULL);
		dc = CreateCompatibleDC(screen);
		HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&out, NULL, 0);
		ReleaseDC(NULL, screen);
		flat = malloc((size_t)marginW * GAME_H * sizeof(WORD));
		if (!dc || !dib || !flat || marginW <= EDGE) { dc = NULL; return NULL; }
		SelectObject(dc, dib);
	}

	static int rgb[GAME_H][3]; // each row's average edge colour, 5 bits a channel
	static int chroma[GAME_H], sortedChroma[2 * WINDOW + 1];
	static int lum[GAME_H], mean[GAME_H], sorted[2 * WINDOW + 1], from[GAME_H], width[GAME_H], eroded[GAME_H];
	static BYTE object[GAME_H];
// Pixel k columns in from the screen's edge on this side, and a 0..248 brightness.
#define EDGE_PX(y, k) frame[(size_t)(bottomUp ? GAME_H - 1 - (y) : (y)) * stride + (side ? BASE_W - 1 - (k) : (k))]
#define BRIGHTNESS(p) ((((p) >> 10) & 31) * 2 + (((p) >> 5) & 31) * 5 + ((p) & 31))
	for (int side = 0; side < 2; side++) {
		// A row's edge brightness is that of its brightest few pixels, so an object that only
		// partly crosses the edge strip (the tip of a ribbon) still stands out.
		for (int y = 0; y < GAME_H; y++) {
			int top[4] = { 0, 0, 0, 0 }, total = 0, vivid = 0;
			for (int k = 0; k < EDGE; k++) {
				WORD p = EDGE_PX(y, k);
				int v = BRIGHTNESS(p);
				total += v;
				int r5 = (p >> 10) & 31, g5 = (p >> 5) & 31, b5 = p & 31;
				vivid += max(r5, max(g5, b5)) - min(r5, min(g5, b5));
				for (int c = 0; c < 3; c++) rgb[y][c] = (k ? rgb[y][c] : 0) + ((p >> (c * 5)) & 31);
				for (int t = 0; t < 4; t++) if (v > top[t]) { int swap = top[t]; top[t] = v; v = swap; }
			}
			lum[y] = (top[0] + top[1] + top[2] + top[3]) / 4;
			mean[y] = total / EDGE;
			chroma[y] = vivid * 8 / EDGE; // average colourfulness, in eighths
			for (int c = 0; c < 3; c++) rgb[y][c] /= EDGE;
			from[y] = y;
		}
		// Rows much brighter than the darker rows around them belong to an object.
		for (int y = 0; y < GAME_H; y++) {
			int n = 0;
			for (int j = max(0, y - WINDOW); j <= min(GAME_H - 1, y + WINDOW); j++) {
				int v = mean[j], at = n;
				while (at > 0 && sorted[at - 1] > v) { sorted[at] = sorted[at - 1]; at--; }
				sorted[at] = v;
				v = chroma[j]; at = n++;
				while (at > 0 && sortedChroma[at - 1] > v) { sortedChroma[at] = sortedChroma[at - 1]; at--; }
				sortedChroma[at] = v;
			}
			// The background level is a low percentile, not the median: at the top and bottom of
			// the screen a ribbon can fill half of the rows in reach. A row stands out by being
			// brighter on average, or by being more colourful (gold against grey engraving, whose
			// thin light lines are as bright as the gold but have no colour).
			object[y] = mean[y] > sorted[n * 3 / 10] * 8 / 5 + 14 ||
				(chroma[y] > sortedChroma[n * 3 / 10] * 8 / 5 + 20 && mean[y] > sorted[n * 3 / 10]);
		}
		for (int y = 0; y < GAME_H;) {
			if (!object[y]) { y++; continue; }
			int a = y, b = y;
			while (b < GAME_H && object[b]) b++;
			a = max(0, a - 3);
			b = min(GAME_H, b + 3);
			if (b - a <= RUN_MAX) {
				for (int r = a; r < b; r++) {
					int below = b + (b - 1 - r), above = a - 1 - (r - a);
					if (below < GAME_H) from[r] = below; else if (above >= 0) from[r] = above;
				}
			}
			y = b;
		}
		// How far in each row's background goes.
		for (int y = 0; y < GAME_H; y++) {
			// The limit sits between the edge's average and its highlights: above a drape's folds,
			// below the shaded rim of a parchment scroll.
			int sy = from[y], limit = (mean[sy] + lum[sy]) / 2 * 7 / 5 + 10, sum = 0, tint[3] = { 0, 0, 0 };
			width[y] = marginW;
			for (int k = EDGE - SPAN; k < marginW; k++) {
				WORD in = EDGE_PX(sy, k), gone = k >= EDGE ? EDGE_PX(sy, k - SPAN) : 0;
				sum += BRIGHTNESS(in) - (k >= EDGE ? BRIGHTNESS(gone) : 0);
				int shift = 0;
				for (int c = 0; c < 3; c++) {
					tint[c] += ((in >> (c * 5)) & 31) - (k >= EDGE ? (gone >> (c * 5)) & 31 : 0);
					shift += abs(tint[c] - rgb[sy][c] * SPAN);
				}
				if (k < EDGE - 1) continue;
				// Something else begins where it gets clearly brighter than the edge, or
				// somewhat brighter and of a different colour (a gold ribbon's shaded tail).
				BOOL brighter = sum > limit * SPAN;
				BOOL tinted = shift > 16 * SPAN && sum > mean[sy] * SPAN * 6 / 5;
				if (brighter || tinted) { width[y] = max(EDGE, k - SPAN + 1); break; }
			}
		}
		// Neighbouring rows must reflect at nearly the same place or the picture shears, so each
		// row takes the smallest width near it, then the widths are smoothed. (Both steps only
		// ever shorten a row's own width.)
		for (int y = 0; y < GAME_H; y++) {
			int m = marginW;
			for (int j = max(0, y - ERODE); j <= min(GAME_H - 1, y + ERODE); j++) m = min(m, width[j]);
			eroded[y] = m;
		}
		for (int y = 0; y < GAME_H; y++) {
			int sum = 0, n = 0;
			for (int j = max(0, y - SMOOTH); j <= min(GAME_H - 1, y + SMOOTH); j++) { sum += eroded[j]; n++; }
			int w = max(EDGE, sum / n), sy = from[y];
			for (int d = 0; d < marginW; d++) {
				int k = d % (2 * w);
				flat[y * marginW + d] = EDGE_PX(sy, k < w ? k : 2 * w - 1 - k);
			}
		}
		// Written at 2x with the usual smoothing and a shade that deepens outward. Column 0 of
		// "flat" is nearest the screen.
		for (int y = 0; y < GAME_H; y++) {
			for (int d = 0; d < marginW; d++) {
				WORD p = flat[y * marginW + d];
				int shade = 215 - 150 * d / marginW; // of 256
				int x = side ? d : marginW - 1 - d;  // position within this margin's picture
				for (int sub = 0; sub < 4; sub++) {
					int towardRight = sub & 1;
					int nd = (towardRight != 0) == (side != 0) ? min(d + 1, marginW - 1) : max(d - 1, 0);
					int ny = sub & 2 ? min(y + 1, GAME_H - 1) : max(y - 1, 0);
					WORD ph = flat[y * marginW + nd], pv = flat[ny * marginW + d], pd = flat[ny * marginW + nd];
					DWORD c = 0;
					for (int shift = 0; shift <= 10; shift += 5) {
						DWORD v = 9 * ((p >> shift) & 31) + 3 * ((ph >> shift) & 31) + 3 * ((pv >> shift) & 31) + ((pd >> shift) & 31);
						c |= (((v >> 1) * shade) >> 8) << (shift == 0 ? 0 : shift == 5 ? 8 : 16);
					}
					out[(size_t)(y * 2 + (sub >> 1)) * marginW * 4 + side * marginW * 2 + x * 2 + towardRight] = c;
				}
			}
		}
	}
#undef EDGE_PX
#undef BRIGHTNESS
	return dc;
}

// Beside a centred 800-wide screen, instead of plain black bars: the screen's own background
// continued outward, the tiled panel texture, or a dimmed, heavily blurred wash of its edge. "src" holds the 1x frame.
static void draw_margins(HDC dst, HDC src)
{
	static HDC small, blurred;
	static DWORD *tiny, *soft;
	RECT left = { -g_offsetX, 0, 0, GAME_H }, right = { BASE_W, 0, BASE_W + g_offsetX, GAME_H };
	BOOL showLeft = RectVisible(dst, &left), showRight = RectVisible(dst, &right);
	if (!showLeft && !showRight) return;
	if (!g_marginArt || !g_screenOnTop) {
		FillRect(dst, &left, (HBRUSH)GetStockObject(BLACK_BRUSH));
		FillRect(dst, &right, (HBRUSH)GetStockObject(BLACK_BRUSH));
		return;
	}
	HDC extended = g_marginArt == 3 ? margin_extension(src) : NULL;
	if (extended) {
		int mode = SetStretchBltMode(dst, g_twoStage ? COLORONCOLOR : HALFTONE);
		if (showLeft) StretchBlt(dst, left.left, 0, g_offsetX, GAME_H, extended, 0, 0, g_offsetX * 2, GAME_H * 2, SRCCOPY);
		if (showRight) StretchBlt(dst, right.left, 0, g_offsetX, GAME_H, extended, g_offsetX * 2, 0, g_offsetX * 2, GAME_H * 2, SRCCOPY);
		SetStretchBltMode(dst, mode);
		return;
	}
	HDC texture = g_marginArt >= 2 ? margin_texture() : NULL;
	if (texture) {
		int mode = SetStretchBltMode(dst, g_twoStage ? COLORONCOLOR : HALFTONE);
		if (showLeft) StretchBlt(dst, left.left, 0, g_offsetX, GAME_H, texture, 0, 0, g_offsetX * 2, GAME_H * 2, SRCCOPY);
		if (showRight) StretchBlt(dst, right.left, 0, g_offsetX, GAME_H, texture, 0, 0, g_offsetX * 2, GAME_H * 2, SRCCOPY);
		SetStretchBltMode(dst, mode);
		return;
	}
	enum { TW = 16, TH = 40, BW = 64, BH = 300 };
	if (!small) {
		BITMAPINFO bi = { 0 };
		bi.bmiHeader.biSize = sizeof bi.bmiHeader;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		HDC screen = GetDC(NULL);
		small = CreateCompatibleDC(screen);
		bi.bmiHeader.biWidth = TW;
		bi.bmiHeader.biHeight = -TH;
		SelectObject(small, CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&tiny, NULL, 0));
		SetStretchBltMode(small, HALFTONE);
		blurred = CreateCompatibleDC(screen);
		bi.bmiHeader.biWidth = BW * 2;
		bi.bmiHeader.biHeight = -BH;
		SelectObject(blurred, CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&soft, NULL, 0));
		ReleaseDC(NULL, screen);
		if (!tiny || !soft) { g_marginArt = 0; return; }
	}
	// Shrink each 48-pixel edge strip to 8x40, then enlarge it again with interpolation and
	// dimmed: a soft wash of the screen's edge colours.
	StretchBlt(small, 0, 0, 8, TH, src, 0, 0, 48, GAME_H, SRCCOPY);
	StretchBlt(small, 8, 0, 8, TH, src, BASE_W - 48, 0, 48, GAME_H, SRCCOPY);
	GdiFlush();
	for (int side = 0; side < 2; side++) {
		for (int y = 0; y < BH; y++) {
			float fy = 1 + (y + 0.5f) * (TH - 2) / BH - 0.5f;
			int y0 = (int)fy, y1 = min(y0 + 1, TH - 1);
			float ty = fy - y0;
			for (int x = 0; x < BW; x++) {
				float fx = side * 8 + 1 + (x + 0.5f) * 6 / BW - 0.5f;
				int x0 = (int)fx, x1 = min(x0 + 1, side * 8 + 7);
				float tx = fx - x0;
				DWORD c = 0;
				for (int shift = 0; shift <= 16; shift += 8) {
					float top = ((tiny[y0 * TW + x0] >> shift) & 255) * (1 - tx) + ((tiny[y0 * TW + x1] >> shift) & 255) * tx;
					float bottom = ((tiny[y1 * TW + x0] >> shift) & 255) * (1 - tx) + ((tiny[y1 * TW + x1] >> shift) & 255) * tx;
					c |= (DWORD)((top * (1 - ty) + bottom * ty) * 0.38f) << shift;
				}
				soft[y * BW * 2 + side * BW + x] = c;
			}
		}
	}
	int mode = SetStretchBltMode(dst, COLORONCOLOR);
	if (showLeft) StretchBlt(dst, left.left, 0, g_offsetX, GAME_H, blurred, 0, 0, BW, BH, SRCCOPY);
	if (showRight) StretchBlt(dst, right.left, 0, g_offsetX, GAME_H, blurred, BW, 0, BW, BH, SRCCOPY);
	SetStretchBltMode(dst, mode);
}
static BOOL (WINAPI *o_LineTo)(HDC, int, int);
static BOOL (WINAPI *o_PatBlt)(HDC, int, int, int, int, DWORD);
static int (WINAPI *o_SetROP2)(HDC, int);
static int (WINAPI *o_FrameRect)(HDC, const RECT *, HBRUSH);

static BOOL drawing_on_view(HDC dc) { return g_drawTrace > 0 && g_view && WindowFromDC(dc) == g_view && g_drawTrace--; }

static BOOL (WINAPI *o_MoveToEx)(HDC, int, int, POINT *);
static BOOL (WINAPI *o_Polyline)(HDC, const POINT *, int);
static int (WINAPI *o_FillRect)(HDC, const RECT *, HBRUSH);
static BOOL (WINAPI *o_Rectangle)(HDC, int, int, int, int);

static BOOL WINAPI h_MoveToEx(HDC dc, int x, int y, POINT *old)
{
	ensure_mapped(dc);
	return o_MoveToEx(dc, x, y, old);
}

static BOOL WINAPI h_Polyline(HDC dc, const POINT *pts, int count)
{
	ensure_mapped(dc);
	return o_Polyline(dc, pts, count);
}

static int WINAPI h_FillRect(HDC dc, const RECT *r, HBRUSH brush)
{
	ensure_mapped(dc);
	return o_FillRect(dc, r, brush);
}

static BOOL WINAPI h_Rectangle(HDC dc, int l, int t, int r, int b)
{
	ensure_mapped(dc);
	return o_Rectangle(dc, l, t, r, b);
}

// A one-pixel line drawn straight onto an enlarged game window is drawn by GDI centred on the
// enlarged pixel's edge, so part of it falls outside the pixels the game later repaints to erase
// it. Horizontal and vertical ones are drawn instead as the exact band of enlarged pixels.
static BOOL scaled_line(HDC dc, int x, int y)
{
	if (!scaled() || !is_game_window(WindowFromDC(dc)) || GetROP2(dc) != R2_COPYPEN) return FALSE;
	POINT from;
	LOGPEN pen;
	HGDIOBJ current = GetCurrentObject(dc, OBJ_PEN);
	if (!GetCurrentPositionEx(dc, &from) || !current || !GetObjectA(current, sizeof pen, &pen)) return FALSE;
	if (pen.lopnStyle != PS_SOLID || pen.lopnWidth.x > 1) return FALSE;
	RECT band;
	if (from.y == y && from.x != x) {
		// LineTo leaves out its end point.
		band.left = from.x < x ? from.x : x + 1;
		band.right = from.x < x ? x : from.x + 1;
		band.top = y;
		band.bottom = y + 1;
	} else if (from.x == x && from.y != y) {
		band.top = from.y < y ? from.y : y + 1;
		band.bottom = from.y < y ? y : from.y + 1;
		band.left = x;
		band.right = x + 1;
	} else {
		return FALSE;
	}
	HBRUSH brush = CreateSolidBrush(pen.lopnColor);
	o_FillRect(dc, &band, brush);
	DeleteObject(brush);
	o_MoveToEx(dc, x, y, NULL);
	return TRUE;
}

static BOOL WINAPI h_LineTo(HDC dc, int x, int y)
{
	ensure_mapped(dc);
	if (scaled_line(dc, x, y)) return TRUE;
	if (drawing_on_view(dc)) { POINT p; GetCurrentPositionEx(dc, &p); logf_("  LineTo view %ld,%ld -> %d,%d rop2 %d", p.x, p.y, x, y, GetROP2(dc)); }
	return o_LineTo(dc, x, y);
}

static BOOL WINAPI h_PatBlt(HDC dc, int x, int y, int w, int h, DWORD rop)
{
	ensure_mapped(dc);
	if (drawing_on_view(dc)) logf_("  PatBlt view %d,%d %dx%d rop %08lx", x, y, w, h, (unsigned long)rop);
	return o_PatBlt(dc, x, y, w, h, rop);
}

static int WINAPI h_SetROP2(HDC dc, int mode)
{
	if (drawing_on_view(dc)) logf_("  SetROP2 view %d", mode);
	return o_SetROP2(dc, mode);
}

static int WINAPI h_FrameRect(HDC dc, const RECT *r, HBRUSH brush)
{
	ensure_mapped(dc);
	if (r && drawing_on_view(dc)) logf_("  FrameRect view %ld,%ld-%ld,%ld rop2 %d", r->left, r->top, r->right, r->bottom, GetROP2(dc));
	return o_FrameRect(dc, r, brush);
}

// Diagnostic: where the game draws text (target bitmap size), counted.
static BOOL (WINAPI *o_ExtTextOutA)(HDC, int, int, UINT, const RECT *, LPCSTR, UINT, const INT *);
static BOOL (WINAPI *o_TextOutA)(HDC, int, int, LPCSTR, int);
static int (WINAPI *o_DrawTextA)(HDC, LPCSTR, int, LPRECT, UINT);
static void note_text(HDC dc, const char *fn, LPCSTR s, int n)
{
	static int logged;
	if (logged > 400) return;
	BITMAP bm = { 0 };
	HGDIOBJ obj = GetCurrentObject(dc, OBJ_BITMAP);
	if (obj) GetObjectA(obj, sizeof bm, &bm);
	LOGFONTA lf = { 0 };
	HGDIOBJ f = GetCurrentObject(dc, OBJ_FONT);
	if (f) GetObjectA(f, sizeof lf, &lf);
	char text[40] = "";
	if (s) { int k = n < 0 ? (int)strlen(s) : n; if (k > 38) k = 38; memcpy(text, s, k); text[k] = 0; }
	logged++;
	logf_("text %s into %ldx%ld %dbpp window %p: font '%s' %ld, mode %d align %u \"%s\"", fn, bm.bmWidth, bm.bmHeight, bm.bmBitsPixel,
		(void *)WindowFromDC(dc), lf.lfFaceName, lf.lfHeight, GetMapMode(dc), GetTextAlign(dc), text);
}
static BOOL WINAPI h_ExtTextOutA(HDC dc, int x, int y, UINT o, const RECT *r, LPCSTR s, UINT n, const INT *dx)
{ if (g_dumpViews) note_text(dc, "ExtTextOutA", s, (int)n); return o_ExtTextOutA(dc, x, y, o, r, s, n, dx); }
static BOOL WINAPI h_TextOutA(HDC dc, int x, int y, LPCSTR s, int n)
{ if (g_dumpViews) note_text(dc, "TextOutA", s, n); return o_TextOutA(dc, x, y, s, n); }
static int text_hd_draw(HDC dc, LPCSTR s, int n, LPRECT r, UINT f, BOOL *handled);
static int WINAPI h_DrawTextA(HDC dc, LPCSTR s, int n, LPRECT r, UINT f)
{
	static int flagLogs;
	if (g_dumpViews && flagLogs++ < 400) { note_text(dc, "DrawTextA", s, n); if (r) logf_("   flags %04x rect %ld,%ld-%ld,%ld bk %d color %06lx", f, r->left, r->top, r->right, r->bottom, GetBkMode(dc), (unsigned long)GetTextColor(dc)); }
	BOOL handled = FALSE;
	int result = text_hd_draw(dc, s, n, r, f, &handled);
	return handled ? result : o_DrawTextA(dc, s, n, r, f);
}
static BOOL WINAPI h_BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
	if (drawing_on_view(dst)) {
		BITMAP bm = { 0 };
		HGDIOBJ obj = src ? GetCurrentObject(src, OBJ_BITMAP) : NULL;
		if (obj) GetObjectA(obj, sizeof bm, &bm);
		logf_("  BitBlt view %d,%d %dx%d from %d,%d of %ldx%ld %dbpp%s rop %08lx", x, y, w, h, sx, sy, bm.bmWidth, bm.bmHeight,
			bm.bmBitsPixel, bm.bmBits ? " dib" : " ddb", (unsigned long)rop);
	}
	ensure_mapped(dst);
	if (g_hd) {
		if (hd_map_to_window(dst, x, y, w, h, src, sx, sy, rop)) return TRUE;
		if (hd_small_to_window(dst, x, y, w, h, src, sx, sy, rop)) return TRUE;
		if (!hd_transfer_map(dst, x, y, w, h, src, sx, sy, rop)) hd_note_gdi_draw(dst, x, y, w, h);
	}
	if (g_traceBudget > 0 && WindowFromDC(dst) == g_view) {
		g_traceBudget--;
		BITMAP bm = { 0 };
		HGDIOBJ obj = src ? GetCurrentObject(src, OBJ_BITMAP) : NULL;
		if (obj) GetObjectA(obj, sizeof bm, &bm);
		RECT clip = { 0 };
		int kind = o_GetClipBox ? o_GetClipBox(dst, &clip) : GetClipBox(dst, &clip);
		logf_("  blit to view %d,%d %dx%d from %d,%d of %ldx%ld bitmap; clip kind %d box %ld,%ld-%ld,%ld", x, y, w, h, sx, sy,
			bm.bmWidth, bm.bmHeight, kind, clip.left, clip.top, clip.right, clip.bottom);
	}

	// Reading the screen back (the blinking unit does this to composite its sprite over what is
	// already there): the enlarged view has to be shrunk into the game's small bitmap. GDI's
	// default shrink mode ANDs pixels together and turns the patch nearly black, so average them.
	if (scaled() && src && is_game_window(WindowFromDC(src))) {
		ensure_mapped(src);
		// HALFTONE into a 256-colour bitmap dithers to the halftone palette, which shows up as
		// coloured noise (the battle screen's corner ornaments read back that way).
		BITMAP into = { 0 };
		HGDIOBJ target = GetCurrentObject(dst, OBJ_BITMAP);
		BOOL deep = target && GetObjectA(target, sizeof into, &into) && into.bmBitsPixel >= 16;
		// HALFTONE reading from the window comes out as coloured noise on some drivers, and
		// averaging gains nothing when the patch is only put back where it came from.
		(void)deep;
		int mode = SetStretchBltMode(dst, COLORONCOLOR);
		if (g_drawTrace > 0) logf_("  readback %d,%d %dx%d from view %d,%d into %ldx%ld %dbpp%s", x, y, w, h, sx, sy,
			into.bmWidth, into.bmHeight, into.bmBitsPixel, into.bmBits ? " dib" : " ddb");
		SetBrushOrgEx(dst, 0, 0, NULL);
		BOOL ok = o_BitBlt(dst, x, y, w, h, src, sx, sy, rop);
		SetStretchBltMode(dst, mode);
		return ok;
	}
	// Copying a piece of the frame buffer to the view: stretch the whole frame through a clip
	// instead, so every partial update lands on the same pixel grid as a full one.
	if (scaled() && src && x == sx && y == sy && WindowFromDC(dst) == g_view && holds_game_frame(src)) {
		int saved = SaveDC(dst);
		// Exactly the rectangle the game asked for: anything more would wipe what it drew
		// straight onto the window next to it (the battle screen's selection brackets) and never
		// restore. Updates that reach a side of a centred screen also repaint that margin.
		int left = x, right = x + w;
		if (g_offsetX && left <= 0) left -= g_offsetX;
		if (g_offsetX && right >= BASE_W) right += g_offsetX;
		IntersectClipRect(dst, left, y, right, y + h);
		// A centred screen only owns the left 800 columns of the frame buffer; the rest still
		// holds whatever the map screen left there, so it is not shown and the margins are black.
		int width = g_offsetX ? BASE_W : GAME_W;
		BOOL ok;
		LARGE_INTEGER t0, t1;
		if (g_timing) QueryPerformanceCounter(&t0);
		// Smooth scaling in two stages: the frame is first doubled with interpolation into the
		// 2x frame (which is also where HD art lives), and that is then enlarged the rest of the
		// way without filtering. GDI's own smooth mode costs 60-90 ms for a full frame here.
		BOOL composed = (g_hd || g_twoStage) && rop == SRCCOPY && hd_compose(src, x - 2, y - 2, x + w + 2, y + h + 2);
		if (g_timing) { LARGE_INTEGER tc; QueryPerformanceCounter(&tc); g_composeTicks += tc.QuadPart - t0.QuadPart; }
		if (composed && quad_present(dst, src, x - 2, y - 2, x + w + 2, y + h + 2, width, rop)) {
			ok = TRUE;
		} else if (composed) {
			int mode = g_twoStage ? SetStretchBltMode(dst, COLORONCOLOR) : 0;
			ok = StretchBlt(dst, 0, 0, width, GAME_H, g_hdDC, 0, 0, width * 2, GAME_H * 2, rop);
			if (mode) SetStretchBltMode(dst, mode);
		} else {
			ok = StretchBlt(dst, 0, 0, width, GAME_H, src, 0, 0, width, GAME_H, rop);
		}
		if (g_timing) {
			GdiFlush();
			QueryPerformanceCounter(&t1);
			g_presentTicks += t1.QuadPart - t0.QuadPart;
			g_presentPixels += w * h;
		}
		if (g_offsetX) draw_margins(dst, src);
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

	// TMacViewMgr::CreateMapArtStorage makes six more sheets with one 64-pixel cell per slot,
	// which the tile drawing code indexes by slot number alongside the cache. Left at 110 cells,
	// slots 110 and up read and write past the end of each row, which shows as stray dashes of
	// colour in freshly drawn tiles.
	static const DWORD sheets[] = { 0x005497ce, 0x00549805, 0x00549822, 0x00549838, 0x00549852, 0x0054988b };
	for (int i = 0; i < (int)(sizeof sheets / sizeof sheets[0]); i++) {
		ok &= patch_imm32(sheets[i], CACHE_OLD_SLOTS * 64, CACHE_ALLOCATED_SLOTS * 64);
	}
	return ok;
}

// Fixed "the map is 576 pixels / 9 tiles wide" numbers in the map code.
static BOOL widen_map_constants(int extra)
{
	// Draw marks a cache slot "on screen" when its tile lies within a span of rows and columns
	// from the view's corner. The row span is this constant (first row + 9, ten rows); the column
	// span is a global, set in widen_globals. With 127 slots the wide view affords 14 columns by
	// 9 rows, so the row span drops by one.
	BOOL ok = patch_imm8(0x0055d22c, 9, 8);
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

// Fixed positions in the drawing code of screens with a layout (layouts.h), moved to match it.
// Each is a "push imm32" found by scanning the function, so that a mismatch is noticed.
static BOOL patch_push_in(DWORD function, DWORD length, DWORD value, DWORD moved)
{
	BYTE *site = NULL;
	int found = 0;
	for (BYTE *p = (BYTE *)function; p + 5 <= (BYTE *)function + length; p++)
		if (p[0] == 0x68 && *(DWORD *)(p + 1) == value) { site = p; found++; }
	if (found != 1) {
		logf_("push %lu in %08lx: found %d times, not patched", (unsigned long)value, (unsigned long)function, found);
		return FALSE;
	}
	return patch_imm32((DWORD)(DWORD_PTR)(site + 1), value, moved);
}

// TDiplomacyMapView::CreateDrawGeometries works out where the map of nations is (from the map
// view, 'here') and compiles drawing code for each country's shape there. It cannot be run a
// second time (the compiled code is appended to, not replaced), so it is made to see the map view
// where the layout will put it while it runs the one time the game calls it.
static BYTE *g_geometriesRest; // its first instructions, then a jump to the rest

static void __attribute__((thiscall)) diplomacy_geometries(BYTE *self)
{
	typedef void (__attribute__((thiscall)) *Fn)(void *);
	BYTE *here = child_view(self, ID4('h', 'e', 'r', 'e'));
	// (Every screen built on this map is widened with it moved: the country shapes are built
	// further right for all of them.)
	BOOL moved = g_diplomacyMapMoved && here && VIEW_INT(self, V_W) == BASE_W;
	if (moved) VIEW_INT(here, V_X) += DIPLOMACY_MAP_SHIFT;
	((Fn)g_geometriesRest)(self);
	if (moved) VIEW_INT(here, V_X) -= DIPLOMACY_MAP_SHIFT; // the layout moves it for good
}

static BOOL hook_diplomacy_geometries(void)
{
	BYTE *site = (BYTE *)0x00527c20;
	static const BYTE expected[] = { 0x6a, 0xff, 0x68 }; // push -1; push <handler>
	if (memcmp(site, expected, sizeof expected)) return FALSE;
	g_geometriesRest = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!g_geometriesRest) return FALSE;
	memcpy(g_geometriesRest, site, 7);
	g_geometriesRest[7] = 0xe9;
	*(int *)(g_geometriesRest + 8) = (int)((site + 7) - (g_geometriesRest + 12));
	DWORD old;
	VirtualProtect(site, 7, PAGE_EXECUTE_READWRITE, &old);
	site[0] = 0xe9;
	*(int *)(site + 1) = (int)((BYTE *)diplomacy_geometries - (site + 5));
	site[5] = site[6] = 0x90;
	VirtualProtect(site, 7, old, &old);
	FlushInstructionCache(GetCurrentProcess(), site, 7);
	return TRUE;
}

static void widen_layout_constants(void)
{
	// TTradeScreenPicture::Draw draws each row's goods icon at x=171, in the table's Item
	// column, which the trade layout moves 128 to the right.
	patch_push_in(0x00653870, 0x3b0, 171, 171 + 128);

	// The diplomacy screens' map of nations is drawn at fixed screen positions: the country
	// regions are built with its top-left corner (120, 265) in AddTileToTinyRgn, and
	// CreateDrawGeometries places names and capitals at (column + 24) * 5. Moved right by 25
	// columns of 5 pixels, as the diplomacy layouts move the map view.
	g_diplomacyMapMoved = hook_diplomacy_geometries() && patch_imm32(0x00547314, 120, 120 + DIPLOMACY_MAP_SHIFT) &&
		patch_imm8(0x00527e86, 0x18, 0x18 + DIPLOMACY_MAP_SHIFT / 5) &&
		patch_imm8(0x0052806d, 0x18, 0x18 + DIPLOMACY_MAP_SHIFT / 5);
	if (!g_diplomacyMapMoved) logf_("diplomacy map positions not as expected: diplomacy screens stay centred");
}

// TTacticalBattleView::DrawDecorativeCorners places the four gilt corner pieces of the battlefield
// at fixed positions in a 775x480 view; the right-hand two move over with the right edge. The
// positions are "mov [esp+disp8], imm32" stores into its local rectangles, except the shared right
// edge (0x307), which is loaded once into a register ("mov ecx, imm32") and stored from there.
static BOOL widen_battle_constants(int extra)
{
	BYTE *start = (BYTE *)0x00628950, *end = start + 0x1c0;
	BYTE *sites[8];
	int found = 0;
	for (BYTE *p = start; p + 8 <= end && found < 8; p++) {
		if (p[0] >= 0xb8 && p[0] <= 0xbf && *(DWORD *)(p + 1) == 0x307) {
			sites[found++] = p + 1;
			continue;
		}
		// C7 /0 with a disp8 operand: ModRM 0x44 needs an SIB byte (esp-based), 0x45 is ebp-based.
		if (p[0] != 0xc7 || (p[1] != 0x44 && p[1] != 0x45)) continue;
		BYTE *imm = p + (p[1] == 0x44 ? 4 : 3);
		DWORD v = *(DWORD *)imm;
		if (v == 0x2ec || v == 0x2d8) sites[found++] = imm;
	}
	if (found != 3) {
		logf_("battle corner positions: found %d sites, expected 3: field stays 775 wide", found);
		return FALSE;
	}
	BOOL ok = TRUE;
	for (int i = 0; i < found; i++) {
		DWORD v = *(DWORD *)sites[i];
		ok &= patch_imm32((DWORD)(DWORD_PTR)sites[i], v, v + extra);
	}
	return ok;
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

// "tight" is a second hash, of the whole picture row by row without padding. The game assembles
// some small 8-bit pictures into sprite sheets, and a cell copied out of a sheet is recognised
// by hashing just that cell the same way. Zero when not computed.
typedef struct { DWORD hash, tight; int w, h, bpp, id; char archive[24]; BYTE *data8; } HdIndexEntry;
typedef struct { DWORD hash; int w, h, bpp; DWORD *pixels; } HdImage; // pixels NULL: known to be missing

static char g_hdAssets[MAX_PATH];
static HdIndexEntry *g_hdIndex;
static int g_hdIndexCount;
static HdImage g_hdImages[512];
static int g_hdMissLog = 40; // diagnostic: unmatched pictures still to be reported
static int g_hdImageCount;


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

#define HD_CELL_MAX 160 // largest picture considered as a sprite-sheet cell

// Hash of a w-by-h region of an 8-bit picture, rows taken top to bottom.
static DWORD tight_hash(const BYTE *bits, int pictureW, int pictureH, BOOL bottomUp, int x, int y, int w, int h)
{
	int stride = (pictureW + 3) & ~3;
	DWORD hash = fnv(2166136261u, (const BYTE *)&w, sizeof w);
	hash = fnv(hash, (const BYTE *)&h, sizeof h);
	for (int row = 0; row < h; row++) {
		hash = fnv(hash, bits + (size_t)(bottomUp ? pictureH - 1 - (y + row) : y + row) * stride + x, (size_t)w);
	}
	return hash ? hash : 1;
}

// Size of a picture's pixel data: rows are padded to 4 bytes.
static size_t pixel_bytes(int w, int h, int bpp)
{
	return bpp == 16 ? (size_t)((w + 1) & ~1) * 2 * h : (size_t)((w + 3) & ~3) * h;
}

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
				if (bih.biSize != 40 || (bih.biBitCount != 16 && bih.biBitCount != 8) || bih.biCompression != BI_RGB) continue;
				int w = bih.biWidth, h = abs(bih.biHeight);
				size_t size = pixel_bytes(w, h, bih.biBitCount), n = size < HD_SLICE ? size : HD_SLICE;
				DWORD colours = bih.biClrUsed ? bih.biClrUsed : (bih.biBitCount == 8 ? 256 : 0);
				DWORD base = offset + 40 + colours * 4;
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
				e->hash = hash; e->w = w; e->h = h; e->bpp = bih.biBitCount; e->id = (int)id;
				e->tight = 0;
				e->data8 = NULL;
				if (bih.biBitCount == 8) {
					// Keep the pixels, unpadded and top-down, for matching by similarity.
					BYTE *whole = malloc(size);
					SetFilePointer(f, base, NULL, FILE_BEGIN);
					if (whole && ReadFile(f, whole, (DWORD)size, &got, NULL) && got == size && (e->data8 = malloc((size_t)w * h))) {
						int rowBytes = (w + 3) & ~3;
						for (int y = 0; y < h; y++) {
							memcpy(e->data8 + (size_t)y * w, whole + (size_t)(bih.biHeight > 0 ? h - 1 - y : y) * rowBytes, (size_t)w);
						}
					}
					free(whole);
				}
				lstrcpynA(e->archive, name, sizeof e->archive);
				if (g_hdIndexCount >= 4096) break;
			}
			free(buf);
		}
		free(dir);
	}
	CloseHandle(f);
}

// The index by hash, for whole-picture lookups, and by size, for sprite-cell matching: scanning
// the whole index on every miss cost about 2 ms per sprite drawn.
static int *g_hdByHash, *g_hdBySize;

static int cmp_by_hash(const void *a, const void *b)
{
	DWORD x = g_hdIndex[*(const int *)a].hash, y = g_hdIndex[*(const int *)b].hash;
	return x < y ? -1 : x > y;
}

static int cmp_by_size(const void *a, const void *b)
{
	const HdIndexEntry *x = &g_hdIndex[*(const int *)a], *y = &g_hdIndex[*(const int *)b];
	int kx = x->w * 65536 + x->h, ky = y->w * 65536 + y->h;
	return kx < ky ? -1 : kx > ky;
}

static void hd_sort_index(void)
{
	g_hdByHash = malloc(sizeof(int) * (g_hdIndexCount + 1));
	g_hdBySize = malloc(sizeof(int) * (g_hdIndexCount + 1));
	if (!g_hdByHash || !g_hdBySize) return;
	for (int i = 0; i < g_hdIndexCount; i++) g_hdByHash[i] = g_hdBySize[i] = i;
	qsort(g_hdByHash, g_hdIndexCount, sizeof(int), cmp_by_hash);
	qsort(g_hdBySize, g_hdIndexCount, sizeof(int), cmp_by_size);
}

// First position in g_hdByHash with that hash, or in g_hdBySize with that size.
static int index_lower_bound(const int *order, BOOL bySize, DWORD key)
{
	int lo = 0, hi = g_hdIndexCount;
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		const HdIndexEntry *e = &g_hdIndex[order[mid]];
		DWORD k = bySize ? (DWORD)(e->w * 65536 + e->h) : e->hash;
		if (k < key) lo = mid + 1; else hi = mid;
	}
	return lo;
}
static void hd_sort_index(void);
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
	hd_sort_index();
	logf_("HD: indexed %d pictures", g_hdIndexCount);
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

// Decoded HD pictures by file, so a picture matched again (a sprite seen over different terrain,
// for one) is not decoded again. The cache owns the pixels; HdImage entries borrow them.
typedef struct { DWORD key; DWORD *pixels; size_t bytes; } PngCacheEntry;
static PngCacheEntry g_pngCache[1024];
static int g_pngCacheCount;
static size_t g_pngCacheBytes;

static DWORD *hd_load_cached(const char *path, int w2, int h2)
{
	DWORD key = fnv(fnv(2166136261u, (const BYTE *)path, strlen(path)), (const BYTE *)&w2, sizeof w2);
	for (int i = 0; i < g_pngCacheCount; i++) if (g_pngCache[i].key == key) return g_pngCache[i].pixels;
	if (g_pngCacheCount >= (int)(sizeof g_pngCache / sizeof g_pngCache[0]) || g_pngCacheBytes > 600u * 1024 * 1024) {
		// Full: everything that refers to cached pictures is forgotten with them.
		g_hdImageCount = 0;
		hd_forget_art();
		for (int i = 0; i < g_pngCacheCount; i++) free(g_pngCache[i].pixels);
		g_pngCacheCount = 0;
		g_pngCacheBytes = 0;
	}
	DWORD *pixels = hd_load_png(path, w2, h2);
	if (pixels) {
		g_pngCache[g_pngCacheCount].key = key;
		g_pngCache[g_pngCacheCount].pixels = pixels;
		g_pngCache[g_pngCacheCount].bytes = (size_t)w2 * h2 * 4;
		g_pngCacheBytes += (size_t)w2 * h2 * 4;
		g_pngCacheCount++;
	}
	return pixels;
}

// Makes room in the table of matched pictures: sprite cells (cheap to match again) go first.
static void hd_make_room(void)
{
	int kept = 0;
	for (int i = 0; i < g_hdImageCount; i++) if (g_hdImages[i].bpp != -8) g_hdImages[kept++] = g_hdImages[i];
	if (kept < g_hdImageCount * 3 / 4) {
		g_hdImageCount = kept;
		return;
	}
	g_hdImageCount = 0;
	hd_forget_art(); // blocks referring to forgotten art are simply redrawn
}

// The upscaled picture for a picture in memory, or NULL.
static const HdImage *hd_find(const BYTE *bits, int w, int h, int bpp)
{
	size_t size = pixel_bytes(w, h, bpp);
	if (!readable(bits, size)) return NULL;
	DWORD hash = picture_hash(bits, size, w, h);
	for (int i = 0; i < g_hdImageCount; i++) {
		if (g_hdImages[i].hash == hash && g_hdImages[i].w == w && g_hdImages[i].h == h && g_hdImages[i].bpp == bpp) {
			return g_hdImages[i].pixels ? &g_hdImages[i] : NULL;
		}
	}
	if (g_hdImageCount >= (int)(sizeof g_hdImages / sizeof g_hdImages[0])) hd_make_room();

	HdImage *img = &g_hdImages[g_hdImageCount++];
	img->hash = hash; img->w = w; img->h = h; img->bpp = bpp; img->pixels = NULL;
	BOOL indexed = FALSE;
	for (int at = g_hdByHash ? index_lower_bound(g_hdByHash, FALSE, hash) : 0; g_hdByHash && at < g_hdIndexCount && g_hdIndex[g_hdByHash[at]].hash == hash; at++) {
		int i = g_hdByHash[at];
		if (g_hdIndex[i].w != w || g_hdIndex[i].h != h || g_hdIndex[i].bpp != bpp) continue;
		indexed = TRUE;
		char path[MAX_PATH];
		snprintf(path, sizeof path, "%s\\%s\\%d.png", g_hdAssets, g_hdIndex[i].archive, g_hdIndex[i].id);
		img->pixels = hd_load_cached(path, w * 2, h * 2);
		logf_("HD: %s\\%d %dx%d %dbpp %s", g_hdIndex[i].archive, g_hdIndex[i].id, w, h, bpp, img->pixels ? "loaded" : "NOT FOUND on disk");
		break;
	}
	if (!indexed && g_hdMissLog > 0) {
		g_hdMissLog--;
		logf_("HD: no archive picture matches a %dx%d %dbpp picture in memory", w, h, bpp);
	}
	return img->pixels ? img : NULL;
}

// The upscaled picture for one cell of an 8-bit sprite sheet in memory, or NULL.
static const HdImage *hd_find_cell(const BYTE *bits, int pictureW, int pictureH, BOOL bottomUp, int x, int y, int w, int h)
{
	if (!readable(bits, pixel_bytes(pictureW, pictureH, 8))) return NULL;
	// The game remaps some palette indices when it loads 8-bit pictures, so the bytes in memory
	// are close to the archive's but not identical. The region is matched by similarity: among
	// archive pictures of the same size, the one agreeing on the most sampled pixels wins.
	// (Every other row of a small cell is plenty to tell pictures apart, at half the cost.)
	int rowStep = max(2, h / 32), stride = (pictureW + 3) & ~3;
	DWORD hash = fnv(2166136261u, (const BYTE *)&w, sizeof w);
	for (int row = 0; row < h; row += rowStep) {
		hash = fnv(hash, bits + (size_t)(bottomUp ? pictureH - 1 - (y + row) : y + row) * stride + x, (size_t)w);
	}
	for (int i = 0; i < g_hdImageCount; i++) {
		if (g_hdImages[i].hash == hash && g_hdImages[i].w == w && g_hdImages[i].h == h && g_hdImages[i].bpp == -8) {
			return g_hdImages[i].pixels ? &g_hdImages[i] : NULL;
		}
	}
	if (g_hdImageCount >= (int)(sizeof g_hdImages / sizeof g_hdImages[0])) hd_make_room();
	HdImage *img = &g_hdImages[g_hdImageCount++];
	img->hash = hash; img->w = w; img->h = h; img->bpp = -8; img->pixels = NULL; // -8: matched as a cell
	int best = -1, bestSame = 0, secondSame = 0, sampled = 0;
	// Four rows first: most archive pictures differ on those already, and only the rest are
	// compared in full (the full comparison of every candidate took about 4 ms per sprite drawn).
	int quickRows[4] = { h / 8, h * 3 / 8, h * 5 / 8, h * 7 / 8 };
	DWORD sizeKey = (DWORD)(w * 65536 + h);
	for (int at = g_hdBySize ? index_lower_bound(g_hdBySize, TRUE, sizeKey) : g_hdIndexCount; at < g_hdIndexCount &&
		(DWORD)(g_hdIndex[g_hdBySize[at]].w * 65536 + g_hdIndex[g_hdBySize[at]].h) == sizeKey; at++) {
		int i = g_hdBySize[at];
		if (g_hdIndex[i].bpp != 8 || !g_hdIndex[i].data8) continue;
		int quick = 0;
		for (int q = 0; q < 4; q++) {
			int row = quickRows[q];
			const BYTE *mem = bits + (size_t)(bottomUp ? pictureH - 1 - (y + row) : y + row) * stride + x;
			const BYTE *file = g_hdIndex[i].data8 + (size_t)row * w;
			for (int col = 0; col < w; col++) quick += mem[col] == file[col];
		}
		if (quick * 100 < 4 * w * 60) continue; // far from the 80% needed
		int same = 0;
		sampled = 0;
		for (int row = 0; row < h; row += rowStep) {
			const BYTE *mem = bits + (size_t)(bottomUp ? pictureH - 1 - (y + row) : y + row) * stride + x;
			const BYTE *file = g_hdIndex[i].data8 + (size_t)row * w;
			for (int col = 0; col < w; col++) same += mem[col] == file[col];
			sampled += w;
		}
		if (same > bestSame) { secondSame = bestSame; bestSame = same; best = i; }
		else if (same > secondSame) secondSame = same;
	}
	for (int i = best; i >= 0 && i == best; i++) {
		if (bestSame * 100 < sampled * 80) break; // under 80% agreement: not the same picture
		// Two archive pictures that both fit (animation frames, colour variants) cannot be told
		// apart reliably unless the winner is near-exact or clearly ahead of the runner-up.
		if (bestSame * 100 < sampled * 97 && (bestSame - secondSame) * 100 < sampled * 5) {
			logf_("HD: %dx%d 8-bit picture is ambiguous (%d%% vs %d%%): left as is", w, h,
				bestSame * 100 / max(sampled, 1), secondSame * 100 / max(sampled, 1));
			break;
		}
		char path[MAX_PATH];
		snprintf(path, sizeof path, "%s\\%s\\%d.png", g_hdAssets, g_hdIndex[i].archive, g_hdIndex[i].id);
		img->pixels = hd_load_cached(path, w * 2, h * 2);
		if (g_hdMissLog > 0 || !img->pixels) {
			g_hdMissLog--;
			logf_("HD: %s\\%d %dx%d 8-bit (%d%% match) %s", g_hdIndex[i].archive, g_hdIndex[i].id, w, h,
				bestSame * 100 / max(sampled, 1), img->pixels ? "loaded" : "NOT FOUND on disk");
		}
		break;
	}
	return img->pixels ? img : NULL;
}

// A surface pairs one of the game's 1x pixel buffers with a frame twice its size. "state" says
// what each 2x block holds: 0 unknown, 1 HD art, 2 smoothed 1x pixel. "shadow" is the 1x value
// the pixel had when its block was written, so later changes by the game can be detected.
//
// There are two: the frame that is shown, and the buffer the map view is assembled in before it
// is copied to the frame (the game's "GWorld", 64 pixels wider and 128 taller than the screen).
typedef struct {
	int w, h;
	const WORD *bits; // the 1x buffer this state describes
	HDC dc;
	DWORD *frame;     // top-down, 2w by 2h
	WORD *shadow;
	BYTE *state, *dirty;
} HdSurface;

#define MAP_SURFACE_H 728
#define HD_KEY 0xff00ffu // transparent pixels in upscaled sprites
static HdSurface g_main, g_map;

static BOOL surface_create(HdSurface *s, int w, int h)
{
	if (s->frame) return s->w == w && s->h == h;
	BITMAPINFO bi = { 0 };
	bi.bmiHeader.biSize = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth = w * 2;
	bi.bmiHeader.biHeight = -h * 2;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	HDC screen = GetDC(NULL);
	s->dc = CreateCompatibleDC(screen);
	HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&s->frame, NULL, 0);
	ReleaseDC(NULL, screen);
	s->shadow = calloc((size_t)w * h, sizeof *s->shadow);
	s->state = calloc((size_t)w * h, 1);
	s->dirty = calloc((size_t)w * h, 1);
	if (!s->dc || !bmp || !s->shadow || !s->state || !s->dirty) { s->frame = NULL; return FALSE; }
	SelectObject(s->dc, bmp);
	s->w = w;
	s->h = h;
	return TRUE;
}

// The game has several offscreen buffers of the same size, so they are told apart by address.
static void surface_track(HdSurface *s, const WORD *bits)
{
	if (bits == s->bits) return;
	s->bits = bits;
	memset(s->state, 0, (size_t)s->w * s->h);
}

static void hd_forget_art(void)
{
	if (g_main.state) memset(g_main.state, 0, (size_t)g_main.w * g_main.h);
	if (g_map.state) memset(g_map.state, 0, (size_t)g_map.w * g_map.h);
}

// ---------------------------------------------------------------------------------------------
// Map tiles
//
// The game builds each map tile at run time (terrain, coast, borders, roads) into a cache of
// 8-bit 64x64 cells, then copies cells into the map buffer. A finished tile is identified by a
// hash of its cell. Tiles with no upscaled version yet are saved to assets\orig\_tiles as they
// are met; tools\upscale_all.ps1 -Only _tiles turns those into assets\x2\_tiles, which is what
// is loaded here.

typedef struct { DWORD hash; DWORD *pixels; BYTE used, onDisk, captured; } HdTile;
#define HD_TILE_SLOTS 16384
#define HD_TILE_MAX 12000
#define HD_TILE_LOADED_MAX 3000
static HdTile *g_tiles;
static int g_tileCount, g_tilesLoaded, g_tilesCaptured;
static BOOL g_captureTiles = TRUE;
static char g_tileArtDir[MAX_PATH], g_tileOrigDir[MAX_PATH];

int WINAPI GdipCreateBitmapFromScan0(INT w, INT h, INT stride, INT format, BYTE *scan0, void **bitmap);
int WINAPI GdipSaveImageToFile(void *image, const WCHAR *path, const CLSID *encoder, const void *params);

static HdTile *tile_slot(DWORD hash)
{
	if (!g_tiles) return NULL;
	for (DWORD i = hash & (HD_TILE_SLOTS - 1);; i = (i + 1) & (HD_TILE_SLOTS - 1)) {
		HdTile *t = &g_tiles[i];
		if (t->used && t->hash == hash) return t;
		if (!t->used) {
			if (g_tileCount >= HD_TILE_MAX) return NULL;
			g_tileCount++;
			t->used = 1;
			t->hash = hash;
			return t;
		}
	}
}

static int scan_tile_dir(const char *dir, BOOL art)
{
	char pattern[MAX_PATH];
	WIN32_FIND_DATAA fd;
	int found = 0;
	snprintf(pattern, sizeof pattern, "%s\\*.png", dir);
	HANDLE find = FindFirstFileA(pattern, &fd);
	if (find == INVALID_HANDLE_VALUE) return 0;
	do {
		char *end;
		DWORD hash = strtoul(fd.cFileName, &end, 16);
		if (end != fd.cFileName + 8) continue;
		HdTile *t = tile_slot(hash);
		if (!t) break;
		if (art) t->onDisk = 1; else t->captured = 1;
		found++;
	} while (FindNextFileA(find, &fd));
	FindClose(find);
	return found;
}

static void hd_scan_tiles(void)
{
	g_tiles = calloc(HD_TILE_SLOTS, sizeof *g_tiles);
	snprintf(g_tileArtDir, sizeof g_tileArtDir, "%s\\_tiles", g_hdAssets);
	snprintf(g_tileOrigDir, sizeof g_tileOrigDir, "%s\\..\\orig\\_tiles", g_hdAssets);
	CreateDirectoryA(g_tileOrigDir, NULL);
	int captured = scan_tile_dir(g_tileOrigDir, FALSE);
	int art = scan_tile_dir(g_tileArtDir, TRUE);
	logf_("HD: map tiles: %d upscaled, %d captured so far; capture %s", art, captured, g_captureTiles ? "on" : "off");
}

static void save_tile(DWORD hash, const WORD *frame, int stride, BOOL bottomUp, int surfaceH, int dx, int dy)
{
	static const CLSID pngEncoder = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
	static ULONG_PTR token;
	static DWORD pixels[64 * 64];
	if (!token) {
		GpStartupInput input = { 1, NULL, FALSE, FALSE };
		if (GdiplusStartup(&token, &input, NULL) != 0) return;
	}
	for (int y = 0; y < 64; y++) {
		const WORD *row = frame + (size_t)(bottomUp ? surfaceH - 1 - (dy + y) : dy + y) * stride + dx;
		for (int x = 0; x < 64; x++) {
			DWORD r = (row[x] >> 10) & 31, g = (row[x] >> 5) & 31, b = row[x] & 31;
			pixels[y * 64 + x] = 0xff000000 | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
		}
	}
	char path[MAX_PATH];
	WCHAR wide[MAX_PATH];
	snprintf(path, sizeof path, "%s\\%08lx.png", g_tileOrigDir, (unsigned long)hash);
	MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);
	void *bitmap = NULL;
	if (GdipCreateBitmapFromScan0(64, 64, 64 * 4, GP_FORMAT_32BPP_RGB, (BYTE *)pixels, &bitmap) != 0 || !bitmap) return;
	if (GdipSaveImageToFile(bitmap, wide, &pngEncoder, NULL) == 0) {
		g_tilesCaptured++;
		if (g_tilesCaptured == 1 || g_tilesCaptured % 100 == 0) logf_("HD: captured %d new map tiles this session", g_tilesCaptured);
	}
	GdipDisposeImage(bitmap);
}

// The upscaled art for the 64x64 cell at (sx, sy) of an 8-bit cache picture, or NULL. The cell
// has just been copied to (dx, dy) of a 16-bit buffer, which is what gets saved if it is new.
static const DWORD *tile_art(const BYTE *bits, int pw, int ph, BOOL bottomUp8, int sx, int sy,
	const WORD *frame, int stride, BOOL bottomUp, int surfaceH, int dx, int dy)
{
	if (!g_tiles || !readable(bits, pixel_bytes(pw, ph, 8))) return NULL;
	HdTile *t = tile_slot(tight_hash(bits, pw, ph, bottomUp8, sx, sy, 64, 64));
	if (!t) return NULL;
	if (t->pixels) return t->pixels;
	if (t->onDisk) {
		if (g_tilesLoaded >= HD_TILE_LOADED_MAX) return NULL;
		char path[MAX_PATH];
		snprintf(path, sizeof path, "%s\\%08lx.png", g_tileArtDir, (unsigned long)t->hash);
		t->pixels = hd_load_png(path, 128, 128);
		if (t->pixels) g_tilesLoaded++; else t->onDisk = 0;
		return t->pixels;
	}
	if (g_captureTiles && !t->captured && frame) {
		t->captured = 1;
		save_tile(t->hash, frame, stride, bottomUp, surfaceH, dx, dy);
	}
	return NULL;
}

static BOOL hd_ready(void)
{
	static BOOL indexed;
	if (!surface_create(&g_main, g_gameW, GAME_H)) return FALSE;
	g_hdDC = g_main.dc;
	if (g_hd && !indexed) {
		indexed = TRUE;
		hd_build_index();
		hd_scan_tiles();
	}
	return TRUE;
}

static void hd_copy_pixels_after(BYTE *src, BYTE *dst, int sx, int sy, int w, int h, int dx, int dy, DWORD transparent);

static void __attribute__((thiscall)) hd_copy_pixels(BYTE *src, BYTE *dst, int sx, int sy, int w, int h,
	int dx, int dy, DWORD transparent)
{
	LARGE_INTEGER c0, c1, c2;
	QueryPerformanceCounter(&c0);
	((CopyPixelsFn)0x00487790)(src, dst, sx, sy, w, h, dx, dy, transparent);
	QueryPerformanceCounter(&c1);
	hd_copy_pixels_after(src, dst, sx, sy, w, h, dx, dy, transparent);
	QueryPerformanceCounter(&c2);
	g_origCopyTicks += c1.QuadPart - c0.QuadPart;
	g_copyTicks += c2.QuadPart - c1.QuadPart;
	g_copyCalls++;
}

static void hd_copy_pixels_after(BYTE *src, BYTE *dst, int sx, int sy, int w, int h, int dx, int dy, DWORD transparent)
{
	if (!readable(src, 0x14) || !readable(dst, 0x14) || !hd_ready()) return;
	const BITMAPINFOHEADER *sh = *(const BITMAPINFOHEADER **)(src + 0x10);
	const BITMAPINFOHEADER *dh = *(const BITMAPINFOHEADER **)(dst + 0x10);
	if (!readable(sh, sizeof *sh) || !readable(dh, sizeof *dh) || dh->biBitCount != 16) return;

	// Only copies into the frame or the map buffer are of interest.
	HdSurface *s = NULL;
	if (dh->biWidth == g_main.w && abs(dh->biHeight) == g_main.h) s = &g_main;
	else if (g_hd && dh->biWidth == g_gameW + 64 && abs(dh->biHeight) == MAP_SURFACE_H &&
		surface_create(&g_map, g_gameW + 64, MAP_SURFACE_H)) s = &g_map;
	if (!s) return;

	int pw = sh->biWidth, ph = abs(sh->biHeight), stride = (dh->biWidth + 1) & ~1;
	BOOL bottomUp = dh->biHeight > 0, bottomUp8 = sh->biHeight > 0;
	const WORD *frame = *(const WORD **)(dst + 0x0c);
	const BYTE *srcBits = *(const BYTE **)(src + 0x0c);
	if (!frame) return;

	// The same clipping the game applies, then to both pictures' bounds.
	if (dx < 0) { sx -= dx; w += dx; dx = 0; }
	if (dy < 0) { sy -= dy; h += dy; dy = 0; }
	if (sx < 0) { dx -= sx; w += sx; sx = 0; }
	if (sy < 0) { dy -= sy; h += sy; sy = 0; }
	if (w > pw - sx) w = pw - sx;
	if (h > ph - sy) h = ph - sy;
	if (w > s->w - dx) w = s->w - dx;
	if (h > s->h - dy) h = s->h - dy;
	if (w <= 0 || h <= 0) return;

	// Plain copies of 8- and 16-bit pictures, and 8-bit copies that skip one palette index, can
	// have HD art: a map tile, the whole picture upscaled, or (8-bit only) the cell being copied.
	LARGE_INTEGER q0, q1; QueryPerformanceCounter(&q0);
	BOOL keyed = transparent != 0xffffffff, eight = sh->biBitCount == 8;
	const DWORD *pixels = NULL;
	int ax = sx, ay = sy, aw = pw;
	if (s == &g_map && eight && !keyed && w == 64 && h == 64) {
		pixels = tile_art(srcBits, pw, ph, bottomUp8, sx, sy, frame, stride, bottomUp, s->h, dx, dy);
		ax = 0; ay = 0; aw = 64;
	} else if (sh->biBitCount == 16 && !keyed) {
		const HdImage *art = hd_find(srcBits, pw, ph, 16);
		if (art) pixels = art->pixels;
	} else if (eight) {
		const HdImage *art = hd_find(srcBits, pw, ph, 8);
		if (!art) {
			art = hd_find_cell(srcBits, pw, ph, bottomUp8, sx, sy, w, h);
			ax = 0; ay = 0; aw = w;
		}
		if (art) pixels = art->pixels;
	}
	QueryPerformanceCounter(&q1);
	g_findTicks += q1.QuadPart - q0.QuadPart;
	if (pixels) surface_track(s, frame);
	else if (frame != s->bits) return;

	BYTE key = (BYTE)transparent;
	for (int y = 0; y < h; y++) {
		const WORD *row = frame + (size_t)(bottomUp ? s->h - 1 - (dy + y) : dy + y) * stride + dx;
		const BYTE *srcRow = eight ? srcBits + (size_t)(bottomUp8 ? ph - 1 - (sy + y) : sy + y) * ((pw + 3) & ~3) + sx : NULL;
		size_t at = (size_t)(dy + y) * s->w + dx;
		if (!pixels) {
			// The game drew something with no HD version over whatever was here.
			if (keyed && srcRow) { for (int x = 0; x < w; x++) if (srcRow[x] != key) s->state[at + x] = 0; }
			else memset(s->state + at, 0, (size_t)w);
			continue;
		}
		if (!keyed) {
			memcpy(s->shadow + at, row, (size_t)w * 2);
			memset(s->state + at, 1, (size_t)w);
			for (int half = 0; half < 2; half++) {
				memcpy(s->frame + (size_t)((dy + y) * 2 + half) * s->w * 2 + dx * 2,
					pixels + (size_t)((ay + y) * 2 + half) * aw * 2 + ax * 2, (size_t)w * 8);
			}
			continue;
		}
		// Keyed: the game skipped the pixels holding the key index, and so do we.
		// The upscaled sprite's outline is rounded at 2x, so within a block some of the four
		// pixels can be sprite and some background (marked with the key colour, magenta).
		for (int x = 0; x < w; x++) {
			const DWORD *from[2];
			DWORD *to[2], solid = HD_KEY;
			for (int half = 0; half < 2; half++) {
				from[half] = pixels + (size_t)((ay + y) * 2 + half) * aw * 2 + (ax + x) * 2;
				to[half] = s->frame + (size_t)((dy + y) * 2 + half) * s->w * 2 + (dx + x) * 2;
				for (int k = 0; k < 2; k++) if ((from[half][k] & 0xffffff) != HD_KEY) solid = from[half][k];
			}
			if (solid == HD_KEY) continue; // nothing of the sprite in this block
			// Only blocks the game itself drew into: anything written beside them would never be
			// cleaned up (nothing changes there in the 1x frame), leaving bits of selection
			// brackets and sprite edges behind.
			if (srcRow[x] == key) continue;
			// Background pixels of a block keep what is there, if what is there is known.
			BOOL keepBackground = s->state[at + x] != 0;
			for (int half = 0; half < 2; half++) {
				for (int k = 0; k < 2; k++) {
					if ((from[half][k] & 0xffffff) != HD_KEY) to[half][k] = from[half][k];
					else if (!keepBackground) to[half][k] = solid;
				}
			}
			s->shadow[at + x] = row[x];
			s->state[at + x] = 1;
		}
	}
}

// Columns [x, x+w) of rows [top, bottom) of a widened picture drawn from its painted-out version,
// into the frame at origin (ox, oy), and into the HD frame at 2x when there is an upscale.
static void generated_fill(short id, BYTE *dst, int x, int w, int top, int bottom, int ox, int oy)
{
	const GeneratedArt *art = generated_art(id);
	if (!art || !readable(dst, 0x14)) return;
	const BITMAPINFOHEADER *dh = *(const BITMAPINFOHEADER **)(dst + 0x10);
	WORD *frame = *(WORD **)(dst + 0x0c);
	if (!readable(dh, sizeof *dh) || dh->biBitCount != 16 || !frame) return;
	int fw = dh->biWidth, fh = abs(dh->biHeight), stride = (fw + 1) & ~1;
	HdSurface *hd = art->px2 && hd_ready() && fw == g_main.w && fh == g_main.h && g_main.bits == frame ? &g_main : NULL;
	BOOL tracked = fw == g_main.w && fh == g_main.h && g_main.bits == frame && g_main.state;
	for (int y = top; y < bottom; y++) {
		int fy = oy + y;
		if (fy < 0 || fy >= fh || y >= GAME_H) continue;
		WORD *row = frame + (size_t)(dh->biHeight > 0 ? fh - 1 - fy : fy) * stride;
		for (int c = x; c < x + w; c++) {
			int fx = ox + c;
			if (fx < 0 || fx >= fw || c >= g_gameW) continue;
			DWORD p = art->px1[(size_t)y * g_gameW + c];
			WORD v = (WORD)(((p >> 19) & 31) << 10 | ((p >> 11) & 31) << 5 | ((p >> 3) & 31));
			row[fx] = v;
			if (hd) {
				for (int half = 0; half < 2; half++)
					memcpy(hd->frame + (size_t)(fy * 2 + half) * hd->w * 2 + fx * 2,
						art->px2 + (size_t)(y * 2 + half) * g_gameW * 2 + c * 2, 8);
				hd->shadow[(size_t)fy * hd->w + fx] = v;
				hd->state[(size_t)fy * hd->w + fx] = 1;
			} else if (tracked) {
				g_main.state[(size_t)fy * g_main.w + fx] = 0;
			}
		}
	}
}

// One pixel of a game picture (8-bit with its colour table, or 16-bit) as a 16-bit 5-5-5 value.
static WORD picture_pixel(const BITMAPINFOHEADER *h, const BYTE *bits, int x, int y)
{
	int w = h->biWidth, ht = abs(h->biHeight);
	int row = h->biHeight > 0 ? ht - 1 - y : y;
	if (h->biBitCount == 16) return ((const WORD *)(bits + (size_t)row * ((w * 2 + 3) & ~3)))[x];
	const RGBQUAD *table = (const RGBQUAD *)((const BYTE *)h + h->biSize);
	RGBQUAD c = table[bits[(size_t)row * ((w + 3) & ~3) + x]];
	return (WORD)((c.rgbRed >> 3) << 10 | (c.rgbGreen >> 3) << 5 | (c.rgbBlue >> 3));
}

static WORD blend555(WORD a, WORD b, int t) // t from 0 (a) to 256 (b)
{
	int r = ((a >> 10 & 31) * (256 - t) + (b >> 10 & 31) * t + 128) >> 8;
	int g = ((a >> 5 & 31) * (256 - t) + (b >> 5 & 31) * t + 128) >> 8;
	int bl = ((a & 31) * (256 - t) + (b & 31) * t + 128) >> 8;
	return (WORD)(r << 10 | g << 5 | bl);
}

static DWORD blend888(DWORD a, DWORD b, int t)
{
	DWORD out = 0;
	for (int shift = 0; shift < 24; shift += 8) {
		int c = ((int)(a >> shift & 255) * (256 - t) + (int)(b >> shift & 255) * t + 128) >> 8;
		out |= (DWORD)c << shift;
	}
	return out | 0xff000000;
}

// Where output column k of n_out falls among n_in source columns: the left one and the blend.
static void stretch_position(int k, int nOut, int nIn, int *i, int *t)
{
	int p = ((2 * k + 1) * nIn * 256) / (2 * nOut) - 128; // in 1/256 of a source column
	if (p < 0) p = 0;
	*i = p >> 8;
	*t = p & 255;
	if (*i >= nIn - 1) { *i = nIn - 1; *t = 0; }
}

// Draws source columns [sx, sx+sw) of a picture's rows [top, bottom) stretched smoothly to dw
// columns at dx in the frame (rows at dy + row), within columns [clipLeft, clipRight). The HD
// frame gets the same stretch of the picture's HD art when there is one.
static void layout_stretch_fill(BYTE *src, BYTE *dst, int sx, int sw, int top, int bottom, int dx, int dy,
	int dw, int clipLeft, int clipRight)
{
	if (!readable(src, 0x14) || !readable(dst, 0x14) || sw < 2) return;
	const BITMAPINFOHEADER *sh = *(const BITMAPINFOHEADER **)(src + 0x10);
	const BITMAPINFOHEADER *dh = *(const BITMAPINFOHEADER **)(dst + 0x10);
	if (!readable(sh, sizeof *sh) || !readable(dh, sizeof *dh) || dh->biBitCount != 16) return;
	if (sh->biBitCount != 8 && sh->biBitCount != 16) return;
	const BYTE *srcBits = *(const BYTE **)(src + 0x0c);
	WORD *frame = *(WORD **)(dst + 0x0c);
	int fw = dh->biWidth, fh = abs(dh->biHeight), stride = (fw + 1) & ~1;
	if (!srcBits || !frame || sx < 0 || sx + sw > sh->biWidth || bottom > abs(sh->biHeight)) return;
	clipLeft = max(clipLeft, max(dx, 0));
	clipRight = min(clipRight, min(dx + dw, fw));

	HdSurface *hd = NULL;
	const DWORD *art = NULL;
	if (hd_ready() && fw == g_main.w && fh == g_main.h && g_main.bits == frame) {
		const HdImage *image = hd_find(srcBits, sh->biWidth, abs(sh->biHeight), sh->biBitCount);
		if (image && image->pixels) { hd = &g_main; art = image->pixels; }
	}
	int pw = sh->biWidth;
	for (int y = top; y < bottom; y++) {
		int fy = dy + y;
		if (fy < 0 || fy >= fh) continue;
		WORD *row = frame + (size_t)(dh->biHeight > 0 ? fh - 1 - fy : fy) * stride;
		for (int x = clipLeft; x < clipRight; x++) {
			int i, t;
			stretch_position(x - dx, dw, sw, &i, &t);
			WORD a = picture_pixel(sh, srcBits, sx + i, y);
			WORD v = t ? blend555(a, picture_pixel(sh, srcBits, sx + i + 1, y), t) : a;
			row[x] = v;
			if (!hd) continue;
			size_t at = (size_t)fy * hd->w + x;
			for (int half = 0; half < 2; half++) {
				const DWORD *from = art + (size_t)(y * 2 + half) * pw * 2;
				DWORD *to = hd->frame + (size_t)(fy * 2 + half) * hd->w * 2;
				for (int k = 0; k < 2; k++) {
					int i2, t2;
					stretch_position((x - dx) * 2 + k, dw * 2, sw * 2, &i2, &t2);
					DWORD c = from[sx * 2 + i2];
					to[x * 2 + k] = t2 ? blend888(c, from[sx * 2 + i2 + 1], t2) : c;
				}
			}
			hd->shadow[at] = v;
			hd->state[at] = 1;
		}
	}
	if (!hd && g_main.bits == frame && fw == g_main.w && fh == g_main.h && g_main.state) {
		// No HD art: these pixels are smoothed from the 1x frame.
		for (int y = top; y < bottom; y++) {
			int fy = dy + y;
			if (fy >= 0 && fy < fh && clipRight > clipLeft) memset(g_main.state + (size_t)fy * g_main.w + clipLeft, 0, (size_t)(clipRight - clipLeft));
		}
	}
}

// Draws a 'Q' insertion: a patch of the picture, mirror-tiled in both directions, into the band
// rows [top, bottom) at frame columns from dx, within [clipLeft, clipRight). The tiling starts at
// the band's top and the insertion's left edge. The HD frame gets the same from the HD art.
static void layout_patch_fill(BYTE *src, BYTE *dst, const LayoutInsert *in, int bandTop, int top, int bottom, int dx, int dy,
	int clipLeft, int clipRight)
{
	if (!readable(src, 0x14) || !readable(dst, 0x14)) return;
	const BITMAPINFOHEADER *sh = *(const BITMAPINFOHEADER **)(src + 0x10);
	const BITMAPINFOHEADER *dh = *(const BITMAPINFOHEADER **)(dst + 0x10);
	if (!readable(sh, sizeof *sh) || !readable(dh, sizeof *dh) || dh->biBitCount != 16) return;
	if (sh->biBitCount != 8 && sh->biBitCount != 16) return;
	const BYTE *srcBits = *(const BYTE **)(src + 0x0c);
	WORD *frame = *(WORD **)(dst + 0x0c);
	int fw = dh->biWidth, fh = abs(dh->biHeight), stride = (fw + 1) & ~1, pw = sh->biWidth;
	if (!srcBits || !frame || in->src < 0 || in->src + in->srcW > pw || in->srcY + in->srcH > abs(sh->biHeight)) return;
	clipLeft = max(clipLeft, max(dx, 0));
	clipRight = min(clipRight, min(dx + in->width, fw));
	HdSurface *hd = NULL;
	const DWORD *art = NULL;
	if (hd_ready() && fw == g_main.w && fh == g_main.h && g_main.bits == frame) {
		const HdImage *image = hd_find(srcBits, pw, abs(sh->biHeight), sh->biBitCount);
		if (image && image->pixels) { hd = &g_main; art = image->pixels; }
	}
	for (int y = top; y < bottom; y++) {
		int fy = dy + y;
		if (fy < 0 || fy >= fh) continue;
		WORD *row = frame + (size_t)(dh->biHeight > 0 ? fh - 1 - fy : fy) * stride;
		int sy = in->srcY + layout_mirror(y - bandTop, in->srcH);
		for (int x = clipLeft; x < clipRight; x++) {
			int k = x - dx;
			WORD v = picture_pixel(sh, srcBits, in->src + layout_mirror(k, in->srcW), sy);
			row[x] = v;
			size_t at = (size_t)fy * g_main.w + x;
			if (!hd) {
				if (g_main.state && g_main.bits == frame) g_main.state[at] = 0;
				continue;
			}
			for (int half = 0; half < 2; half++) {
				int sy2 = in->srcY * 2 + layout_mirror((y - bandTop) * 2 + half, in->srcH * 2);
				for (int kk = 0; kk < 2; kk++)
					hd->frame[(size_t)(fy * 2 + half) * hd->w * 2 + x * 2 + kk] =
						art[(size_t)sy2 * pw * 2 + in->src * 2 + layout_mirror(k * 2 + kk, in->srcW * 2)];
			}
			hd->shadow[at] = v;
			hd->state[at] = 1;
		}
	}
}
// The 16-bit pixel buffer selected into a device context, with its layout; NULL if there is none.
static const WORD *dc_pixels(HDC dc, int *w, int *h, int *stride, BOOL *bottomUp)
{
	DIBSECTION ds;
	HGDIOBJ bmp = dc ? GetCurrentObject(dc, OBJ_BITMAP) : NULL;
	if (!bmp || GetObjectA(bmp, sizeof ds, &ds) != sizeof ds || !ds.dsBm.bmBits || ds.dsBm.bmBitsPixel != 16) return NULL;
	*w = ds.dsBm.bmWidth;
	*h = ds.dsBm.bmHeight;
	*stride = ds.dsBm.bmWidthBytes / 2;
	*bottomUp = ds.dsBmih.biHeight > 0;
	return ds.dsBm.bmBits;
}

// A drawing rectangle in device pixels, limited to the clip box and the surface.
static RECT device_rect(HDC dc, const HdSurface *s, int x, int y, int w, int h)
{
	POINT pts[2] = { { x, y }, { x + w, y + h } };
	LPtoDP(dc, pts, 2);
	RECT r = { min(pts[0].x, pts[1].x), min(pts[0].y, pts[1].y), max(pts[0].x, pts[1].x), max(pts[0].y, pts[1].y) }, clip;
	if (GetClipBox(dc, &clip) != ERROR) {
		POINT c[2] = { { clip.left, clip.top }, { clip.right, clip.bottom } };
		LPtoDP(dc, c, 2);
		r.left = max(r.left, c[0].x); r.top = max(r.top, c[0].y);
		r.right = min(r.right, c[1].x); r.bottom = min(r.bottom, c[1].y);
	}
	r.left = max(r.left, 0); r.top = max(r.top, 0);
	r.right = min(r.right, s->w); r.bottom = min(r.bottom, s->h);
	return r;
}

// The game has drawn into a rectangle of a surface through GDI (pop-ups, sprites, borders).
// Whatever HD art was there is gone, even where a new pixel happens to have the same value as
// the old one, so the unchanged-value test must not be trusted for these pixels.
static void hd_note_gdi_draw(HDC dst, int x, int y, int w, int h)
{
	int bw, bh, stride;
	BOOL bottomUp;
	const WORD *bits = g_main.frame ? dc_pixels(dst, &bw, &bh, &stride, &bottomUp) : NULL;
	HdSurface *s = !bits ? NULL : bits == g_main.bits ? &g_main : bits == g_map.bits ? &g_map : NULL;
	if (!s) return;
	RECT r = device_rect(dst, s, x, y, w, h);
	for (int row = r.top; row < r.bottom && r.right > r.left; row++) {
		memset(s->state + (size_t)row * s->w + r.left, 0, (size_t)(r.right - r.left));
	}
}

// The game is copying part of the map buffer into the frame: HD blocks go with it.
static BOOL hd_transfer_map(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
	int dw, dh, dstride, mw, mh, mstride;
	BOOL dBottomUp, mBottomUp;
	if (rop != SRCCOPY || !g_map.frame || !g_main.frame) return FALSE;
	const WORD *mapBits = dc_pixels(src, &mw, &mh, &mstride, &mBottomUp);
	if (!mapBits || mapBits != g_map.bits || dc_pixels(dst, &dw, &dh, &dstride, &dBottomUp) != g_main.bits) return FALSE;

	POINT d = { x, y }, m = { sx, sy };
	LPtoDP(dst, &d, 1);
	LPtoDP(src, &m, 1);
	RECT r = device_rect(dst, &g_main, x, y, w, h);
	for (int row = r.top; row < r.bottom; row++) {
		int my = m.y + (row - d.y);
		if (my < 0 || my >= g_map.h) continue;
		const WORD *mapRow = mapBits + (size_t)(mBottomUp ? g_map.h - 1 - my : my) * mstride;
		for (int col = r.left; col < r.right; col++) {
			int mx = m.x + (col - d.x);
			size_t at = (size_t)row * g_main.w + col;
			if (mx < 0 || mx >= g_map.w) { g_main.state[at] = 0; continue; }
			size_t mat = (size_t)my * g_map.w + mx;
			if (g_map.state[mat] != 1 || g_map.shadow[mat] != mapRow[mx]) { g_main.state[at] = 0; continue; }
			g_main.state[at] = 1;
			g_main.shadow[at] = mapRow[mx];
			for (int half = 0; half < 2; half++) {
				const DWORD *from = g_map.frame + (size_t)(my * 2 + half) * g_map.w * 2 + mx * 2;
				DWORD *to = g_main.frame + (size_t)(row * 2 + half) * g_main.w * 2 + col * 2;
				to[0] = from[0];
				to[1] = from[1];
			}
		}
	}
	return TRUE;
}

// Brings a surface's 2x frame up to date for a rectangle of its 1x buffer.
static void surface_compose(HdSurface *s, const WORD *frame, int stride, BOOL bottomUp, int x0, int y0, int x1, int y1)
{
	x0 = max(x0, 0); y0 = max(y0, 0); x1 = min(x1, s->w); y1 = min(y1, s->h);
	if (x1 <= x0 || y1 <= y0) return;
#define FRAME_ROW(yy) (frame + (size_t)(bottomUp ? s->h - 1 - (yy) : (yy)) * stride)

	// Pass 1: find the pixels the game has changed since their 2x block was written.
	for (int y = y0; y < y1; y++) {
		const WORD *row = FRAME_ROW(y);
		size_t at = (size_t)y * s->w;
		for (int x = x0; x < x1; x++) {
			WORD p = row[x];
			if (s->state[at + x] && s->shadow[at + x] == p) continue;
			s->state[at + x] = 2;
			s->shadow[at + x] = p;
			s->dirty[at + x] = 1;
		}
	}

	// Pass 2: redraw the non-HD blocks that changed or touch a change. Each 1x pixel becomes four,
	// each a 9:3:3:1 mix of it and its three neighbours on that side, which smooths text and
	// sprites instead of doubling their pixels.
	int bx0 = max(x0 - 1, 0), by0 = max(y0 - 1, 0), bx1 = min(x1 + 1, s->w), by1 = min(y1 + 1, s->h);
	for (int y = by0; y < by1; y++) {
		const WORD *rows[3] = { FRAME_ROW(max(y - 1, 0)), FRAME_ROW(y), FRAME_ROW(min(y + 1, s->h - 1)) };
		size_t at = (size_t)y * s->w;
		DWORD *out = s->frame + (size_t)y * 2 * s->w * 2;
		for (int x = bx0; x < bx1; x++) {
			if (s->state[at + x] != 2) continue;
			BOOL touched = FALSE;
			for (int ny = max(y - 1, 0); ny <= min(y + 1, s->h - 1) && !touched; ny++) {
				for (int nx = max(x - 1, 0); nx <= min(x + 1, s->w - 1); nx++) {
					if (s->dirty[(size_t)ny * s->w + nx]) { touched = TRUE; break; }
				}
			}
			if (!touched) continue;
			WORD p = rows[1][x];
			for (int sub = 0; sub < 4; sub++) {
				int nx = sub & 1 ? min(x + 1, s->w - 1) : max(x - 1, 0);
				const WORD *other = rows[sub & 2 ? 2 : 0];
				WORD ph = rows[1][nx], pv = other[x], pd = other[nx];
				DWORD c = 0;
				for (int shift = 0; shift <= 10; shift += 5) {
					DWORD v = 9 * ((p >> shift) & 31) + 3 * ((ph >> shift) & 31) + 3 * ((pv >> shift) & 31) + ((pd >> shift) & 31);
					v >>= 1; // 0..496 (16 x 31) -> 0..248, the same 5-to-8-bit expansion GDI uses
					c |= v << (shift == 0 ? 0 : shift == 5 ? 8 : 16);
				}
				out[(sub & 2 ? s->w * 2 : 0) + x * 2 + (sub & 1)] = c;
			}
		}
	}
	for (int y = y0; y < y1; y++) memset(s->dirty + (size_t)y * s->w + x0, 0, (size_t)(x1 - x0));
#undef FRAME_ROW
}

// Brings the frame that is shown up to date for a rectangle, given the DC holding the 1x frame.
static BOOL hd_compose(HDC source, int x0, int y0, int x1, int y1)
{
	int w, h, stride;
	BOOL bottomUp;
	const WORD *frame = hd_ready() ? dc_pixels(source, &w, &h, &stride, &bottomUp) : NULL;
	if (!frame || w != g_main.w || h != g_main.h) return FALSE;
	surface_track(&g_main, frame);
	surface_compose(&g_main, frame, stride, bottomUp, x0, y0, x1, y1);
	return TRUE;
}

// ---------------------------------------------------------------------------------------------
// Full-screen paintings at the display's own resolution
//
// A screen that is a single painted-out picture (layouts.h, 'G', with assets\x4\_wide art) is not
// shown through the 2x frame. Its 4x art is scaled once to the size of the game's window, and each
// update is composed at that size: wherever the game's frame still shows the painting's own pixel,
// the window gets the painting; anywhere the game has drawn something else (text, highlights),
// it gets the usual 2x frame.

static HDC g_viewArtDC, g_viewComposeDC;
static DWORD *g_viewArt, *g_viewCompose;
static int g_viewArtW, g_viewArtH;
static short g_viewArtId;

static HDC dib_dc(int w, int h, DWORD **bits)
{
	BITMAPINFO bi = { 0 };
	bi.bmiHeader.biSize = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)bits, NULL, 0);
	if (!bmp) return NULL;
	HDC dc = CreateCompatibleDC(NULL);
	SelectObject(dc, bmp);
	return dc;
}

static void free_dib_dc(HDC *dc)
{
	if (!*dc) return;
	HGDIOBJ bmp = GetCurrentObject(*dc, OBJ_BITMAP);
	DeleteDC(*dc);
	DeleteObject(bmp);
	*dc = NULL;
}

// The painting at the window's size, made when the screen or the window size changes.
static BOOL prepare_view_art(const GeneratedArt *art, short id)
{
	if (g_viewArtDC && g_viewArtId == id && g_viewArtW == g_viewW && g_viewArtH == g_viewH) return TRUE;
	free_dib_dc(&g_viewArtDC);
	free_dib_dc(&g_viewComposeDC);
	DWORD *bits4;
	HDC four = dib_dc(g_gameW * 4, GAME_H * 4, &bits4);
	if (!four) return FALSE;
	memcpy(bits4, art->px4, (size_t)g_gameW * 4 * GAME_H * 4 * 4);
	g_viewArtDC = dib_dc(g_viewW, g_viewH, &g_viewArt);
	g_viewComposeDC = dib_dc(g_viewW, g_viewH, &g_viewCompose);
	if (g_viewArtDC) {
		SetStretchBltMode(g_viewArtDC, HALFTONE);
		SetBrushOrgEx(g_viewArtDC, 0, 0, NULL);
		StretchBlt(g_viewArtDC, 0, 0, g_viewW, g_viewH, four, 0, 0, g_gameW * 4, GAME_H * 4, SRCCOPY);
		GdiFlush();
	}
	free_dib_dc(&four);
	if (!g_viewArtDC || !g_viewComposeDC) return FALSE;
	g_viewArtId = id;
	g_viewArtW = g_viewW;
	g_viewArtH = g_viewH;
	logf_("painting %d prepared at the window's size, %dx%d", id, g_viewW, g_viewH);
	return TRUE;
}

static BOOL quad_present(HDC dst, HDC src, int x0, int y0, int x1, int y1, int width, DWORD rop)
{
	if (!g_hd || !g_hdNative || g_offsetX || !g_topScreen || !g_main.frame || rop != SRCCOPY || width != g_gameW) return FALSE;
	WidePicture *wide = wide_picture(g_topScreen);
	if (!wide || wide->picture != g_topScreen || !wide->look.generated || VIEW_INT(g_topScreen, 0x2c) != 0) return FALSE;
	const GeneratedArt *art = generated_art(wide->look.generated);
	if (!art || !art->px4 || !prepare_view_art(art, wide->look.generated)) return FALSE;
	int fw, fh, stride;
	BOOL bottomUp;
	const WORD *frame = dc_pixels(src, &fw, &fh, &stride, &bottomUp);
	if (!frame || fw != g_gameW || fh != GAME_H) return FALSE;

	int W = g_gameW, H = GAME_H;
	x0 = max(x0, 0); y0 = max(y0, 0); x1 = min(x1, W); y1 = min(y1, H);
	if (x1 <= x0 || y1 <= y0) return TRUE;
	// The window pixels covering the game pixels being updated.
	int vx0 = (int)((long long)x0 * g_viewW / W), vx1 = (int)(((long long)x1 * g_viewW + W - 1) / W);
	int vy0 = (int)((long long)y0 * g_viewH / H), vy1 = (int)(((long long)y1 * g_viewH + H - 1) / H);
	vx1 = min(vx1, g_viewW); vy1 = min(vy1, g_viewH);
	for (int vy = vy0; vy < vy1; vy++) {
		int gy = (int)((long long)vy * H / g_viewH);
		const WORD *row = frame + (size_t)(bottomUp ? H - 1 - gy : gy) * stride;
		const DWORD *expected = art->px1 + (size_t)gy * W;
		const DWORD *from2 = g_main.frame + (size_t)((long long)vy * H * 2 / g_viewH) * W * 2;
		DWORD *out = g_viewCompose + (size_t)vy * g_viewW;
		const DWORD *painted = g_viewArt + (size_t)vy * g_viewW;
		for (int vx = vx0; vx < vx1; vx++) {
			int gx = (int)((long long)vx * W / g_viewW);
			DWORD p = expected[gx];
			WORD e = (WORD)(((p >> 19) & 31) << 10 | ((p >> 11) & 31) << 5 | ((p >> 3) & 31));
			out[vx] = row[gx] == e ? painted[vx] : from2[(long long)vx * W * 2 / g_viewW];
		}
	}
	GdiFlush();
	// One window pixel per composed pixel: the window's device context maps game coordinates to
	// it, so the copy is given in game units with the composed picture's own size as the source.
	int mode = SetStretchBltMode(dst, COLORONCOLOR);
	BOOL ok = StretchBlt(dst, 0, 0, W, H, g_viewComposeDC, 0, 0, g_viewW, g_viewH, rop);
	SetStretchBltMode(dst, mode);
	return ok;
}

// ---------------------------------------------------------------------------------------------
// Text at 2x
//
// The game draws all its text with DrawTextA into the 1x frame, in scalable fonts. Each call is
// also drawn into the 2x frame with the font at twice the size, so text is drawn sharp instead of
// enlarged. The 2x text is kept only on the 1x pixels the 1x text changed, which are then marked
// as HD art: when the game draws over them again they change and are redrawn as usual.

static BOOL g_sharpText = TRUE;
static HDC g_textDC;
static DWORD *g_textBits;

typedef struct { LOGFONTA from; HFONT font; } TextFont;
static TextFont g_textFonts[32];
static int g_textFontCount;

static HFONT text_font_2x(HDC dc)
{
	LOGFONTA lf;
	HGDIOBJ f = GetCurrentObject(dc, OBJ_FONT);
	if (!f || !GetObjectA(f, sizeof lf, &lf)) return NULL;
	for (int i = 0; i < g_textFontCount; i++)
		if (!memcmp(&g_textFonts[i].from, &lf, sizeof lf)) return g_textFonts[i].font;
	LOGFONTA big = lf;
	big.lfHeight *= 2;
	big.lfWidth *= 2;
	big.lfQuality = ANTIALIASED_QUALITY;
	HFONT font = CreateFontIndirectA(&big);
	if (font && g_textFontCount < (int)(sizeof g_textFonts / sizeof g_textFonts[0])) {
		g_textFonts[g_textFontCount].from = lf;
		g_textFonts[g_textFontCount].font = font;
		g_textFontCount++;
	}
	return font;
}

// Draws text into g_textDC at 2x with every character at twice its 1x position: the 2x font's
// own spacing drifts from the 1x one along a line, and the 2x text is only kept where the 1x text
// landed. Lines are broken the way DrawText breaks them (at spaces, and at line feeds), measured
// in the 1x font; (L, T) is where DrawText puts the block.
static void text_layout_2x(HDC dc, LPCSTR s, int n, const RECT *r, UINT f, int L, int T)
{
	if (n < 0) n = (int)strlen(s);
	TEXTMETRICA tm;
	GetTextMetricsA(dc, &tm);
	int lineH = tm.tmHeight + (f & DT_EXTERNALLEADING ? tm.tmExternalLeading : 0);
	int width = r->right - r->left;
	BOOL wrap = (f & DT_WORDBREAK) && !(f & DT_SINGLELINE);
	SetTextAlign(g_textDC, TA_LEFT | TA_TOP | TA_NOUPDATECP);
	static INT ext[1024], dx[1024];
	int y = T, i = 0, lines = 0;
	while (i <= n && lines < 64) {
		// One line: up to a line feed, or as many words as fit.
		int start = i, end = i, best = -1;
		while (end < n && s[end] != '\n' && s[end] != '\r') {
			if (wrap && end > start) {
				SIZE sz;
				GetTextExtentPoint32A(dc, s + start, end + 1 - start, &sz);
				if (sz.cx > width && best > start) break;
			}
			if (s[end] == ' ') best = end;
			end++;
		}
		int stop = end;
		if (end < n && s[end] != '\n' && s[end] != '\r' && best > start) stop = best; // wrapped at a space
		int len = stop - start;
		// (Trailing spaces are kept: DrawText counts them when it aligns a line.)
		if (len > 0 && len < 1024) {
			SIZE sz;
			int fit = 0;
			GetTextExtentExPointA(dc, s + start, len, 0x7fffffff, &fit, ext, &sz);
			int x = f & DT_CENTER ? (r->left + r->right - sz.cx) / 2 : f & DT_RIGHT ? r->right - sz.cx : r->left;
			if (!wrap && (f & DT_SINGLELINE)) x = L;
			for (int k = 0; k < len; k++) dx[k] = 2 * (ext[k] - (k ? ext[k - 1] : 0));
			ExtTextOutA(g_textDC, x * 2, y * 2, 0, NULL, s + start, (UINT)len, dx);
		}
		y += lineH;
		lines++;
		// Past the line feed (CR LF counts once), or past the space the line was broken at.
		i = stop;
		if (i < n && s[i] == '\r') i++;
		if (i < n && s[i] == '\n') i++;
		else if (i < n && s[i] == ' ') i++;
		if (i >= n) break;
		if (f & DT_SINGLELINE) break;
	}
}
static int text_hd_draw(HDC dc, LPCSTR s, int n, LPRECT r, UINT f, BOOL *handled)
{
	*handled = FALSE;
	if (!g_sharpText || !s || !r || (f & DT_CALCRECT) || !(g_hd || g_twoStage) || !hd_ready()) return 0;
	int w, h, stride;
	BOOL bottomUp;
	WORD *frame = (WORD *)dc_pixels(dc, &w, &h, &stride, &bottomUp);
	if (!frame || w != g_main.w || h != g_main.h) return 0;
	if (GetMapMode(dc) != MM_TEXT) return 0;

	// Where the text can land: its measured block placed as DrawText places it, plus a margin.
	RECT c = *r;
	o_DrawTextA(dc, s, n, &c, f | DT_CALCRECT);
	int cw = c.right - c.left, ch = c.bottom - c.top;
	int L = f & DT_CENTER ? (r->left + r->right - cw) / 2 : f & DT_RIGHT ? r->right - cw : r->left;
	int T = (f & DT_SINGLELINE) && (f & DT_VCENTER) ? (r->top + r->bottom - ch) / 2 :
		(f & DT_SINGLELINE) && (f & DT_BOTTOM) ? r->bottom - ch : r->top;
	RECT box = { min(L, r->left) - 4, min(T, r->top) - 4, max(L + cw, r->right) + 4, max(T + ch, r->bottom) + 4 };
	if (!(f & DT_NOCLIP)) { box.left = max(box.left, r->left - 1); box.top = max(box.top, r->top - 1);
		box.right = min(box.right, r->right + 1); box.bottom = min(box.bottom, r->bottom + 1); }
	RECT clip;
	if (GetClipBox(dc, &clip) != ERROR) IntersectRect(&box, &box, &clip);
	box.left = max(box.left, 0); box.top = max(box.top, 0); box.right = min(box.right, w); box.bottom = min(box.bottom, h);
	int bw = box.right - box.left, bh = box.bottom - box.top;
	if (bw <= 0 || bh <= 0 || bw * bh > 400000) return 0;

	// The 2x frame up to date under the text, and the 1x pixels before it.
	hd_compose(dc, box.left, box.top, box.right, box.bottom);
	static WORD *before;
	static int beforeSize;
	if (bw * bh > beforeSize) { free(before); beforeSize = bw * bh; before = malloc(sizeof(WORD) * beforeSize); }
	if (!before) return 0;
#define TEXT_ROW(yy) (frame + (size_t)(bottomUp ? h - 1 - (yy) : (yy)) * stride)
	for (int y = 0; y < bh; y++) memcpy(before + (size_t)y * bw, TEXT_ROW(box.top + y) + box.left, sizeof(WORD) * bw);

	*handled = TRUE;
	int result = o_DrawTextA(dc, s, n, r, f);
	GdiFlush();

	// The same text at 2x, over a copy of the 2x frame.
	if (!g_textDC) g_textDC = dib_dc(g_main.w * 2, g_main.h * 2, &g_textBits);
	HFONT font = g_textDC ? text_font_2x(dc) : NULL;
	if (!font) return result;
	int W2 = g_main.w * 2;
	for (int y = box.top * 2; y < box.bottom * 2; y++)
		memcpy(g_textBits + (size_t)y * W2 + box.left * 2, g_main.frame + (size_t)y * W2 + box.left * 2, sizeof(DWORD) * bw * 2);
	int saved = SaveDC(g_textDC);
	SelectObject(g_textDC, font);
	SetTextColor(g_textDC, GetTextColor(dc));
	SetBkColor(g_textDC, GetBkColor(dc));
	SetBkMode(g_textDC, GetBkMode(dc));
	SetTextAlign(g_textDC, GetTextAlign(dc));
	SetTextCharacterExtra(g_textDC, GetTextCharacterExtra(dc) * 2);
	IntersectClipRect(g_textDC, box.left * 2, box.top * 2, box.right * 2, box.bottom * 2);
	text_layout_2x(dc, s, n, r, f, L, T);
	GdiFlush();
	RestoreDC(g_textDC, saved);

	// Checked before it is used: the 2x text must land where the 1x text did. Each 1x pixel the
	// text changed should hold 2x ink (the 2x block differs from what was there), and 2x ink should
	// not fall on many pixels the 1x text left alone. A font that does not scale evenly (a bitmap
	// font, or one whose 2x glyphs are shaped differently) fails, and keeps its 1x text.
	{
		int changed = 0, inked = 0, stray = 0;
		for (int y = 0; y < bh; y++) {
			const WORD *row = TEXT_ROW(box.top + y) + box.left;
			const WORD *was = before + (size_t)y * bw;
			for (int x = 0; x < bw; x++) {
				size_t o = (size_t)(box.top + y) * 2 * W2 + (box.left + x) * 2;
				BOOL ink = g_textBits[o] != g_main.frame[o] || g_textBits[o + 1] != g_main.frame[o + 1] ||
					g_textBits[o + W2] != g_main.frame[o + W2] || g_textBits[o + W2 + 1] != g_main.frame[o + W2 + 1];
				if (row[x] != was[x]) { changed++; inked += ink; }
				else if (ink) stray++;
			}
		}
		if (changed == 0 || inked * 100 < changed * 70 || stray * 100 > changed * 20) {
			static int rejectLogs;
			if (g_dumpViews && rejectLogs++ < 300) logf_("   2x text not used: %d changed, %d with 2x ink, %d stray", changed, inked, stray);
			return result;
		}
	}
	// Kept where the 1x text changed a pixel.
	int changedCount = 0;	for (int y = 0; y < bh; y++) {
		const WORD *row = TEXT_ROW(box.top + y) + box.left;
		const WORD *was = before + (size_t)y * bw;
		size_t at = (size_t)(box.top + y) * g_main.w + box.left;
		for (int x = 0; x < bw; x++) {
			if (row[x] == was[x]) continue;
			int fx = (box.left + x) * 2, fy = (box.top + y) * 2;
			for (int half = 0; half < 2; half++) {
				size_t o = (size_t)(fy + half) * W2 + fx;
				g_main.frame[o] = g_textBits[o];
				g_main.frame[o + 1] = g_textBits[o + 1];
			}
			g_main.shadow[at + x] = row[x];
			g_main.state[at + x] = 1;
			changedCount++;
		}
	}
	static int textLogs;
	if (g_dumpViews && textLogs++ < 300) logf_("   2x text: box %ld,%ld-%ld,%ld, %d pixels changed, at %d,%d", box.left, box.top, box.right, box.bottom, changedCount, L, T);
#undef TEXT_ROW
	return result;
}// The game sometimes copies a piece of the map buffer straight to the window (the hover outline,
// the blinking unit). Show it from the map's 2x frame so those tiles do not drop out of HD.
// Two 5-5-5 colours that differ by at most four steps in each channel. The blink picture is drawn
// with a slightly different palette from the map (more saturated), so only the highlight outline,
// which is far from anything on the map, fails to match.
static BOOL near555(WORD a, WORD b)
{
	for (int shift = 0; shift <= 10; shift += 5)
		if (abs((int)((a >> shift) & 31) - (int)((b >> shift) & 31)) > 4) return FALSE;
	return TRUE;
}

static int g_mapToBufferX, g_mapToBufferY;
static BOOL g_mapToBufferKnown;

static BOOL hd_map_to_window(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
	int mw, mh, stride;
	BOOL bottomUp;
	if (rop != SRCCOPY || !g_map.frame || !g_view || WindowFromDC(dst) != g_view) return FALSE;
	const WORD *mapBits = dc_pixels(src, &mw, &mh, &stride, &bottomUp);
	if (!mapBits || mapBits != g_map.bits) return FALSE;
	POINT m = { sx, sy };
	LPtoDP(src, &m, 1);
	surface_compose(&g_map, mapBits, stride, bottomUp, m.x, m.y, m.x + w, m.y + h);
	// Where the window's pixels are in the map buffer, for the selection blink.
	g_mapToBufferX = m.x - x;
	g_mapToBufferY = m.y - y;
	g_mapToBufferKnown = TRUE;
	int mode = g_twoStage ? SetStretchBltMode(dst, COLORONCOLOR) : 0;
	BOOL ok = StretchBlt(dst, x, y, w, h, g_map.dc, m.x * 2, m.y * 2, w * 2, h * 2, SRCCOPY);
	if (mode) SetStretchBltMode(dst, mode);
	return ok;
}

// The selected unit blinks by copying small pictures straight to the window, alternately a
// 16-bit picture the game composed itself (tile plus highlighted unit) and the tile's 8-bit cache
// cell. Shown as they are, that tile would flip between HD and low resolution on every blink and
// its stippled shadows would look different from the rest of the map. Instead the picture is
// rebuilt at 2x here: pixels that match what the map buffer holds at that spot take the map's HD
// block, the rest are smoothed like any other non-HD pixel.
#define MAP_VIEW_X 10 // the land map view's position in the frame
#define MAP_VIEW_Y 58
#define SMALL_MAX 256

static BOOL hd_small_to_window(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
	static HDC scratchDC;
	static DWORD *scratch; // top-down, 2*SMALL_MAX square
	DIBSECTION ds;
	// Only on the world map screen: elsewhere the map buffer has nothing to do with what is shown.
	if (rop != SRCCOPY || g_offsetX || !g_mapOnTop || !g_map.frame || !g_view || WindowFromDC(dst) != g_view) return FALSE;
	if (w <= 0 || h <= 0 || w > SMALL_MAX || h > SMALL_MAX) return FALSE;
	HGDIOBJ bmp = GetCurrentObject(src, OBJ_BITMAP);
	if (!bmp || GetObjectA(bmp, sizeof ds, &ds) != sizeof ds || !ds.dsBm.bmBits) return FALSE;
	const BYTE *bits = ds.dsBm.bmBits;
	if ((const WORD *)bits == g_map.bits || (const WORD *)bits == g_main.bits) return FALSE;
	int bw = ds.dsBm.bmWidth, bh = ds.dsBm.bmHeight;
	BOOL bottomUp = ds.dsBmih.biHeight > 0;
	if (sx < 0 || sy < 0 || sx + w > bw || sy + h > bh) return FALSE;

	if (!scratchDC) {
		BITMAPINFO bi = { 0 };
		bi.bmiHeader.biSize = sizeof bi.bmiHeader;
		bi.bmiHeader.biWidth = SMALL_MAX * 2;
		bi.bmiHeader.biHeight = -SMALL_MAX * 2;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		HDC screen = GetDC(NULL);
		scratchDC = CreateCompatibleDC(screen);
		HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&scratch, NULL, 0);
		ReleaseDC(NULL, screen);
		if (!scratchDC || !dib) { scratchDC = NULL; return FALSE; }
		SelectObject(scratchDC, dib);
	}

	if (ds.dsBm.bmBitsPixel == 8) {
		// A tile's cache cell: use its upscaled version if there is one.
		if (w != 64 || h != 64) return FALSE;
		// The row order reported for these bitmaps is not reliable, so both are tried.
		const DWORD *art = tile_art(bits, bw, bh, bottomUp, sx, sy, NULL, 0, FALSE, 0, 0, 0);
		if (!art) art = tile_art(bits, bw, bh, !bottomUp, sx, sy, NULL, 0, FALSE, 0, 0, 0);
		if (!art) return FALSE;
		for (int row = 0; row < 128; row++) memcpy(scratch + (size_t)row * SMALL_MAX * 2, art + row * 128, 128 * 4);
	} else if (ds.dsBm.bmBitsPixel == 16) {
		const WORD *pixels = (const WORD *)bits;
		int stride = ds.dsBm.bmWidthBytes / 2;
		// Where this rectangle lies in the map buffer, which has a 64-pixel margin.
		// (The map buffer scrolls: its offset is the one last used to show it.)
		int mx0 = g_mapToBufferKnown ? x + g_mapToBufferX : x - MAP_VIEW_X + 64;
		int my0 = g_mapToBufferKnown ? y + g_mapToBufferY : y - MAP_VIEW_Y + 64;
		// The row order reported for these bitmaps is not reliable: ask GDI for a few pixels and
		// see which order agrees with them.
		int agree[2] = { 0, 0 };
		for (int i = 0; i < 12; i++) {
			int px = (i * 37 + 5) % w, py = (i * 23 + 3) % h;
			COLORREF c = GetPixel(src, sx + px, sy + py);
			if (c == CLR_INVALID) continue;
			WORD want = (WORD)((GetRValue(c) >> 3) << 10 | (GetGValue(c) >> 3) << 5 | GetBValue(c) >> 3);
			agree[0] += pixels[(size_t)(sy + py) * stride + sx + px] == want;
			agree[1] += pixels[(size_t)(bh - 1 - (sy + py)) * stride + sx + px] == want;
		}
		if (agree[0] == agree[1]) return FALSE; // cannot tell: leave it to the plain copy
		bottomUp = agree[1] > agree[0];
#define SRC_AT(xx, yy) pixels[(size_t)(bottomUp ? bh - 1 - (sy + (yy)) : sy + (yy)) * stride + sx + (xx)]
		for (int row = 0; row < h; row++) {
			for (int col = 0; col < w; col++) {
				WORD p = SRC_AT(col, row);
				int mx = mx0 + col, my = my0 + row;
				DWORD *out = scratch + (size_t)row * 2 * SMALL_MAX * 2 + col * 2;
				if (mx >= 0 && my >= 0 && mx < g_map.w && my < g_map.h) {
					size_t at = (size_t)my * g_map.w + mx;
					if (g_map.state[at] == 1 && near555(g_map.shadow[at], p)) {
						const DWORD *from = g_map.frame + (size_t)my * 2 * g_map.w * 2 + mx * 2;
						out[0] = from[0]; out[1] = from[1];
						out[SMALL_MAX * 2] = from[g_map.w * 2]; out[SMALL_MAX * 2 + 1] = from[g_map.w * 2 + 1];
						continue;
					}
				}
				for (int sub = 0; sub < 4; sub++) {
					int nc = sub & 1 ? min(col + 1, w - 1) : max(col - 1, 0);
					int nr = sub & 2 ? min(row + 1, h - 1) : max(row - 1, 0);
					WORD ph = SRC_AT(nc, row), pv = SRC_AT(col, nr), pd = SRC_AT(nc, nr);
					DWORD c = 0;
					for (int shift = 0; shift <= 10; shift += 5) {
						DWORD v = 9 * ((p >> shift) & 31) + 3 * ((ph >> shift) & 31) + 3 * ((pv >> shift) & 31) + ((pd >> shift) & 31);
						c |= (v >> 1) << (shift == 0 ? 0 : shift == 5 ? 8 : 16);
					}
					out[(sub & 2 ? SMALL_MAX * 2 : 0) + (sub & 1)] = c;
				}
			}
		}
#undef SRC_AT
	} else {
		return FALSE;
	}
	int mode = g_twoStage ? SetStretchBltMode(dst, COLORONCOLOR) : 0;
	BOOL ok = StretchBlt(dst, x, y, w, h, scratchDC, 0, 0, w * 2, h * 2, SRCCOPY);
	if (mode) SetStretchBltMode(dst, mode);
	return ok;
}

// When the game closes after meeting new map tiles, upscale them in the background so they are
// HD on the next run. Runs tools\upscale_all.ps1 from the folder the assets live in.
static BOOL g_autoUpscale = TRUE;

static void hd_on_exit(void)
{
	if (!g_hd || !g_autoUpscale || g_tilesCaptured == 0) return;
	char script[MAX_PATH], command[2 * MAX_PATH + 128];
	snprintf(script, sizeof script, "%s\\..\\..\\tools\\upscale_all.ps1", g_hdAssets);
	if (GetFileAttributesA(script) == INVALID_FILE_ATTRIBUTES) {
		logf_("HD: %d new map tiles captured; run upscale_all.ps1 -Only _tiles to upscale them", g_tilesCaptured);
		return;
	}
	snprintf(command, sizeof command,
		"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%s\" -Only _tiles -NewOnly", script);
	STARTUPINFOA si = { sizeof si };
	PROCESS_INFORMATION pi;
	if (CreateProcessA(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		logf_("HD: upscaling %d new map tiles in the background", g_tilesCaptured);
	} else {
		logf_("HD: could not start the tile upscaler (error %lu)", GetLastError());
	}
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
	g_wideBattle = widen_battle_constants(width - BASE_W);
	build_bg_layouts();
	hook_layout_draw();
	if (g_layoutDrawHooked) widen_layout_constants();
}

// Globals the game fills in before WinMain; adjusted once its windows exist.
static void widen_globals(void)
{
	if (g_gameW <= BASE_W) return;
	int *screenW = (int *)0x0075bc88, *columns = (int *)0x0075c048, *cachedColumnSpan = (int *)0x0075c0d0;
	logf_("globals before: screen width %d, map columns+1 %d, cached row span %d", *screenW,
		(short)*columns, (short)*cachedColumnSpan);
	*screenW = g_gameW; // also sizes the map's composition buffer
	if (g_wideMap) {
		*columns = MAP_COLUMNS_WIDE + 1; // limits how far right the map can scroll
		// Columns whose cache slots count as on screen: first column + 13, fourteen in all (it
		// was 10 for the 9-column view). The game only un-draws the hover outline on a tile
		// whose slot is marked on screen, so too small a span leaves outlines behind.
		*cachedColumnSpan = MAP_COLUMNS_WIDE;
		short *minimapColumns = (short *)0x0076091c; // minimap rectangle width for the close-up map
		logf_("minimap close-up columns before: %d", *minimapColumns);
		if (*minimapColumns == 10) *minimapColumns = MAP_COLUMNS_WIDE + 1; // stored as columns + 1
	}
}

#define HOOK(dll, name) hook_import(dll, #name, h_##name, &o_##name)

// Diagnostic: access violations are logged with where they happened (the game may handle some
// itself; those are logged too, as the first few seen).
static LONG CALLBACK log_crash(EXCEPTION_POINTERS *e)
{
	static int logged;
	if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && logged++ < 5) {
		CONTEXT *c = e->ContextRecord;
		HMODULE mod = NULL;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)e->ExceptionRecord->ExceptionAddress, &mod);
		char name[MAX_PATH] = "?";
		if (mod) GetModuleFileNameA(mod, name, sizeof name);
		const DWORD *stack = (const DWORD *)c->Esp;
		logf_("ACCESS VIOLATION at %p (%s base %p) %s %08lx; eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx",
			e->ExceptionRecord->ExceptionAddress, strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name, (void *)mod,
			e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
			(unsigned long)e->ExceptionRecord->ExceptionInformation[1], c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
		if (readable(stack, 64)) {
			char line[256];
			int at = 0;
			for (int i = 0; i < 16; i++) at += snprintf(line + at, sizeof line - at, " %08lx", (unsigned long)stack[i]);
			logf_("  stack:%s", line);
		}
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

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
	g_timing = GetPrivateProfileIntA("Debug", "Timing", 0, ini);
	g_twoStage = g_smooth && GetPrivateProfileIntA("Display", "FastScale", 1, ini);
	g_captureTiles = GetPrivateProfileIntA("Display", "HDCaptureTiles", 1, ini);
	g_autoUpscale = GetPrivateProfileIntA("Display", "HDAutoUpscale", 1, ini);
	g_marginArt = GetPrivateProfileIntA("Display", "MarginArt", 3, ini);
	g_hdNative = GetPrivateProfileIntA("Display", "HDNative", 1, ini);
	g_sharpText = GetPrivateProfileIntA("Display", "SharpText", 1, ini);
	g_wideSteps = GetPrivateProfileIntA("Debug", "WideSteps", 7, ini);
	if (GetPrivateProfileIntA("Debug", "LogCrashes", 1, ini)) AddVectoredExceptionHandler(1, log_crash);

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
	HOOK("GDI32.dll", MoveToEx);
	HOOK("GDI32.dll", LineTo);
	HOOK("GDI32.dll", Polyline);
	HOOK("GDI32.dll", Rectangle);
	HOOK("GDI32.dll", PatBlt);
	HOOK("USER32.dll", FillRect);
	HOOK("USER32.dll", FrameRect);
	if (g_dumpViews) HOOK("GDI32.dll", SetROP2);
	HOOK("GDI32.dll", ExtTextOutA);
	HOOK("GDI32.dll", TextOutA);
	HOOK("USER32.dll", DrawTextA);
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
