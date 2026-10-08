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
static HWND g_frame, g_view;
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

static void map_dc(HDC dc)
{
	SetMapMode(dc, MM_ANISOTROPIC);
	SetWindowExtEx(dc, GAME_W, GAME_H, NULL);
	SetViewportExtEx(dc, g_viewW, g_viewH, NULL);
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
static struct { HWND hwnd; int x, y, cx, cy; } g_children[16];

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
			if (!(pos->flags & SWP_NOMOVE)) { pos->x = to_view_x(pos->x); pos->y = to_view_y(pos->y); }
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
	child_slot(child, TRUE);
	SetPropA(child, kChildProcProp, (HANDLE)SetWindowLongA(child, GWL_WNDPROC, (LONG)child_proc));

	// It was created at game coordinates; re-apply them so they pass through child_proc.
	RECT r;
	GetWindowRect(child, &r);
	MapWindowPoints(NULL, parent, (POINT *)&r, 2);
	char cls[64] = "";
	GetClassNameA(child, cls, sizeof cls);
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
	if (x) *x = v.left + to_view_x(*x - v.left);
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

static void dump_map_screen_if_new(void)
{
	static const BYTE *last;
	const BYTE **app = (const BYTE **)0x00759888;
	if (!g_knownBuild || !readable(*app, 0x4c)) return;
	const BYTE *picture = *(const BYTE **)(*app + 0x48);
	if (!picture || picture == last || !readable(picture, 0x54)) return;
	last = picture;

	const BYTE *root = picture;
	for (int i = 0; i < 16 && readable(*(const BYTE **)(root + 0x20), 0x54); i++) root = *(const BYTE **)(root + 0x20);
	int budget = 600;
	logf_("--- view tree (map picture %p) ---", (void *)picture);
	dump_view(root, 0, &budget);
	logf_("--- end of view tree ---");
}

static LRESULT CALLBACK view_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_TIMER && wp == VIEW_DUMP_TIMER) {
		dump_map_screen_if_new();
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
			lp = MAKELPARAM(to_game_x((short)LOWORD(lp)), to_game_y((short)HIWORD(lp)));
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
		if (g_dumpViews) SetTimer(g_view, VIEW_DUMP_TIMER, 1500, NULL);
		place_view();
	}
	return hwnd;
}

static HDC WINAPI h_BeginPaint(HWND hwnd, PAINTSTRUCT *ps)
{
	HDC dc = o_BeginPaint(hwnd, ps);
	if (dc && is_game_window(hwnd) && scaled()) {
		map_dc(dc);
		DPtoLP(dc, (POINT *)&ps->rcPaint, 2);
		InflateRect(&ps->rcPaint, 1, 1);
	}
	return dc;
}

static HDC WINAPI h_GetDC(HWND hwnd)
{
	HDC dc = o_GetDC(hwnd);
	if (dc && is_game_window(hwnd) && scaled()) map_dc(dc);
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
	if (ok && is_game_window(hwnd) && scaled()) { p->x = to_game_x(p->x); p->y = to_game_y(p->y); }
	return ok;
}

static BOOL WINAPI h_ClientToScreen(HWND hwnd, POINT *p)
{
	if (p && is_game_window(hwnd) && scaled()) { p->x = to_view_x(p->x); p->y = to_view_y(p->y); }
	return o_ClientToScreen(hwnd, p);
}

static BOOL WINAPI h_InvalidateRect(HWND hwnd, const RECT *r, BOOL erase)
{
	RECT v;
	if (r && is_game_window(hwnd) && scaled()) { v = *r; rect_to_view(&v); r = &v; }
	return o_InvalidateRect(hwnd, r, erase);
}

static BOOL WINAPI h_ValidateRect(HWND hwnd, const RECT *r)
{
	RECT v;
	if (r && is_game_window(hwnd) && scaled()) { v = *r; rect_to_view(&v); r = &v; }
	return o_ValidateRect(hwnd, r);
}

static BOOL WINAPI h_RedrawWindow(HWND hwnd, const RECT *r, HRGN rgn, UINT flags)
{
	RECT v;
	if (r && is_game_window(hwnd) && scaled()) { v = *r; rect_to_view(&v); r = &v; }
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
	if (GetMapMode(dc) == MM_ANISOTROPIC && GetViewportExtEx(dc, &ext) && ext.cx == g_viewW && ext.cy == g_viewH) return;
	map_dc(dc);
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
	return o_StretchDIBits(dst, x, y, w, h, sx, sy, sw, sh, bits, info, usage, rop);
}

static BOOL WINAPI h_BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
	ensure_mapped(dst);

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
		IntersectClipRect(dst, x - 1, y - 1, x + w + 1, y + h + 1);
		BOOL ok = StretchBlt(dst, 0, 0, GAME_W, GAME_H, src, 0, 0, GAME_W, GAME_H, rop);
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

// Wide mode, step one: make the game's own idea of the screen wider than 800. Addresses are for
// the v1.03 exe.
static void widen_screen(int width)
{
	BOOL ok = patch_imm32(0x004aadc4, BASE_W, width);  // GetMainDevice: emulated Mac screen rect
	ok &= patch_imm32(0x0049615e, BASE_W, width);      // CIncludeView::OnDraw: frame buffer size
	ok &= patch_imm32(0x004a2abf, BASE_W, width);      // TWindow::Center: centring against the screen
	if (ok) g_gameW = width;
	logf_("wide mode %s (%d wide)", ok ? "on" : "FAILED", width);
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
	HOOK("GDI32.dll", StretchDIBits);
	BOOL native = skip_mode_switch();
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
