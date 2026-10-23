// Wide layouts for screens designed for 800x600.
//
// A screen's background picture is lengthened by inserting columns at chosen places, and its
// views move with the columns inserted to their left. Each layout splits the picture into
// horizontal bands; within a band, every insertion adds `width` columns before column `at`, filled
// from the stretch of plain artwork starting at `src`:
//   'T' repeats that stretch (for ornament with a known repeat; srcW is the period), carrying on
//       from the column before the insertion
//   'U' the same, timed to run into the column at the insertion instead (for an insertion where
//       a band of ornament starts, so the join is hidden at its end)
//   'M' repeats it mirrored every other time (for shaded artwork, such as a ribbon, whose ends
//       would not meet in a plain repeat)
//   'R' takes short runs of it in a scrambled order (for paper, wood and other texture, where an
//       exact repeat would show)
//   'S' stretches it: the stretch, which must start or end at `at`, is drawn `width` columns wider
//       (for fabric and shading, where any repeat of a fold would show)
// Every band of a screen inserts the same total, LAYOUT_EXTRA. Views are shifted by the insertions
// to the left of their left edge and widened by those under them, taken from the band holding
// their centre. A picture widened that way (a greyed-out row, say) gets a layout of its own made
// from the insertions under it.
//
// This file has no dependencies so that the preview tool (layouttool.c) builds from it too.

#define LAYOUT_EXTRA 256
#define LAYOUT_MAX_INSERTS 4
#define LAYOUT_MAX_BANDS 8
#define LAYOUT_MAX_W 1056

typedef struct { short at, width, src, srcW; char mode; } LayoutInsert;
typedef struct { short top, bottom; LayoutInsert ins[LAYOUT_MAX_INSERTS]; } LayoutBand;
typedef struct {
	const char *name;           // for logs and the preview tool
	const char *cls;            // the picture's class
	unsigned long id;           // its view id
	unsigned long child;        // a child view only this screen of the class has (0: any)
	short width, height;        // the picture's size in the 800x600 layout
	LayoutBand bands[LAYOUT_MAX_BANDS];
	// A picture view can show different artwork at different times (the industry page, one per
	// tab). An entry with `picture` set gives the fill for that artwork only. Views are placed by
	// the plain entry, so its insertions must not be far from that one's: no view may lie between.
	short picture;
	// Pictures over the background keep their size and move with their middle, instead of being
	// lengthened like the background under them (for panels with no plain stretch of their own).
	char fixedPictures;
	// A game routine (thiscall, no arguments, on the screen) that works out drawing positions
	// from the views; called again once they have been moved.
	unsigned long rebuild;
} ScreenLayout;

#define LID(a, b, c, d) (((unsigned long)(a) << 24) | ((unsigned long)(b) << 16) | ((unsigned long)(c) << 8) | (unsigned long)(d))

static const ScreenLayout g_screenLayouts[] = {
	{
		"transport", "TBackgroundPicture", LID('m', 'a', 'i', 'n'), LID('w', 'o', 'o', 'l'), 800, 600, {
			// Banner, knobs and globe: the plain ribbon either side of the globe.
			{ 0, 112, { { 147, 128, 147, 64, 'M' }, { 565, 128, 565, 64, 'M' } } },
			// The frieze along the top of the parchment.
			{ 112, 149, { { 200, 128, 200, 64, 'M' }, { 565, 128, 565, 64, 'M' } } },
			// The rows, at their ends: the sliders draw a bar of fixed length, so they keep their
			// size and the table is centred in the wider parchment. The fill is plain row from
			// behind each column's quantity text. (Not inside a row: a row draws its bar at a fixed
			// offset from its own left edge, so a row must move as a whole.)
			{ 149, 552, { { 167, 128, 212, 16, 'M' }, { 629, 128, 456, 16, 'M' } } },
			// The parchment's lower edge, either side of the ribbon below it.
			{ 552, 600, { { 170, 128, 170, 32, 'M' }, { 604, 128, 604, 32, 'M' } } },
		},
	},
	{
		"industry", "TCityProductionView", LID('m', 'a', 'i', 'n'), LID('g', 'o', 'Q', 'Q'), 800, 600, {
			// Banner: the plain ribbon either side of the plaque (left of the portrait, and under
			// the help text, which is lengthened with it).
			{ 0, 76, { { 194, 128, 194, 32, 'M' }, { 600, 128, 600, 32, 'M' } } },
			// The blue curtain of goods, between each post and the first and last goods.
			{ 76, 190, { { 181, 128, 181, 3, 'S' }, { 617, 128, 609, 8, 'S' } } },
			// Between the rollers. This is under the page, which has a layout of its own; only the
			// torn top edge shows.
			{ 190, 561, { { 114, 128, 114, 32, 'M' }, { 680, 128, 648, 32, 'M' } } },
			// The parchment's lower edge, either side of the ribbon.
			{ 561, 600, { { 170, 128, 170, 32, 'M' }, { 604, 128, 604, 32, 'M' } } },
		},
	},
	{
		"trade", "TTradeScreenPicture", LID('m', 'a', 'i', 'n'), LID('c', 'o', 'm', 'a'), 800, 600, {
			// Banner: the ribbons either side of the cherub (between the treasury and the
			// portrait, and under the help text, which is lengthened with it).
			{ 0, 110, { { 161, 128, 161, 30, 'S' }, { 660, 128, 660, 20, 'S' } } },
			// The frieze, at its ends: the column headings on it stay over their columns.
			{ 110, 141, { { 146, 128, 200, 97, 'U' }, { 648, 128, 400, 97, 'T' } } },
			// The table: its narrow first column and the plain border after its last.
			{ 141, 553, { { 150, 128, 150, 12, 'S' }, { 641, 128, 641, 6, 'S' } } },
			// The parchment's lower edge, either side of the ribbon.
			{ 553, 600, { { 170, 128, 170, 32, 'M' }, { 604, 128, 604, 32, 'M' } } },
		},
	},
	{
		// Diplomacy, on its usual background: a white panel in an ornate frame over a sea chart.
		// The panel is lengthened inside its plain interior, so the panel views over it (one per
		// tab, swapped in and out) grow with it; the chart is lengthened clear of its sun and
		// moon, either side of the map of nations.
		"diplomacy (off)", "TDiplomacyMapView-disabled", LID('m', 'a', 'i', 'n'), LID('h', 'e', 'r', 'e'), 800, 600, {
			{ 0, 55, { { 135, 128, 135, 20, 'S' }, { 660, 128, 640, 20, 'S' } } },
			// The frame's floral border along the top.
			{ 55, 96, { { 135, 128, 140, 40, 'R' }, { 660, 128, 600, 40, 'R' } } },
			// The panel's interior.
			{ 96, 216, { { 135, 128, 135, 64, 'M' }, { 660, 128, 596, 64, 'M' } } },
			// The frame's lower edge and the gold rope under it.
			{ 216, 245, { { 135, 128, 160, 8, 'T' }, { 660, 128, 600, 8, 'T' } } },
			{ 245, 600, { { 100, 128, 80, 20, 'S' }, { 705, 128, 705, 20, 'S' } } },
		},
		// The Information panel is a picture filled to its edges (its colour key), and the map of
		// nations is laid out by TDiplomacyMapView::CreateDrawGeometries from the map view.
		0, 1, 0x00527c20,
	},
	{
		// The same screen on its other background, a red cloth (the same places, other fill).
		"diplomacy (cloth, off)", "TDiplomacyMapView-disabled", LID('m', 'a', 'i', 'n'), 0, 800, 600, {
			{ 0, 55, { { 135, 128, 135, 20, 'S' }, { 660, 128, 640, 20, 'S' } } },
			{ 55, 96, { { 135, 128, 135, 20, 'S' }, { 660, 128, 640, 20, 'S' } } },
			{ 96, 216, { { 135, 128, 135, 20, 'S' }, { 660, 128, 640, 20, 'S' } } },
			{ 216, 245, { { 135, 128, 160, 11, 'T' }, { 660, 128, 600, 11, 'T' } } },
			{ 245, 600, { { 100, 128, 80, 20, 'S' }, { 705, 128, 705, 20, 'S' } } },
		},
		4100,
	},
	// (No layout for the technology screen: both its panels are filled edge to edge by controls
	// and its frame is too thin to take the width, so it stays centred.)
	{
		// The industry screen's page (workers, production or shipyard): its margins, outside the
		// rows, which draw their bars at a fixed place.
		"industry page", "TBackgroundPicture", LID('p', 'a', 'g', 'e'), 0, 595, 361, {
			{ 0, 361, { { 14, 128, 14, 4, 'S' }, { 582, 128, 578, 4, 'S' } } },
		},
	},
	{
		// The shipyard page has panels with a double frame where the others have plain margin:
		// the left panel's inside and the margin beyond the right panels are stretched instead.
		"industry page (shipyard)", "TBackgroundPicture", LID('p', 'a', 'g', 'e'), 0, 595, 361, {
			{ 0, 361, { { 19, 128, 19, 8, 'S' }, { 590, 128, 586, 4, 'S' } } },
		},
		9200,
	},
};

// Columns a layout adds (the same in every band).
static int layout_extra(const ScreenLayout *s)
{
	int total = 0;
	for (int i = 0; i < LAYOUT_MAX_INSERTS && s->bands[0].ins[i].width; i++) total += s->bands[0].ins[i].width;
	return total;
}

static int layout_valid(const ScreenLayout *s)
{
	int y = 0, extra = layout_extra(s);
	for (int b = 0; b < LAYOUT_MAX_BANDS && s->bands[b].bottom; b++) {
		const LayoutBand *band = &s->bands[b];
		if (band->top != y || band->bottom <= band->top) return 0;
		int total = 0, last = -1;
		for (int i = 0; i < LAYOUT_MAX_INSERTS && band->ins[i].width; i++) {
			const LayoutInsert *in = &band->ins[i];
			if (in->at <= last || in->at > s->width || in->srcW <= 0 || in->src < 0 || in->src + in->srcW > s->width) return 0;
			if (in->mode == 'S' && in->src != in->at && in->src + in->srcW != in->at) return 0;
			last = in->at;
			total += in->width;
		}
		if (total != extra) return 0;
		y = band->bottom;
	}
	return y == s->height && s->width + extra <= LAYOUT_MAX_W;
}

static const LayoutBand *layout_band(const ScreenLayout *s, int y)
{
	const LayoutBand *band = &s->bands[0];
	for (int b = 0; b < LAYOUT_MAX_BANDS && s->bands[b].bottom; b++) {
		band = &s->bands[b];
		if (y < band->bottom) break;
	}
	return band;
}

// The layout of a picture inside a widened screen, at x,y in it and w by h: the screen's
// insertions under the picture, moved into its coordinates. Fails if one of them would fill from
// outside the picture.
static int layout_derive(const ScreenLayout *s, int x, int y, int w, int h, ScreenLayout *out)
{
	const LayoutBand *band = layout_band(s, y + h / 2);
	ScreenLayout d = { s->name, 0, 0, 0, (short)w, (short)h, { { 0, (short)h } } };
	int n = 0;
	for (int i = 0; i < LAYOUT_MAX_INSERTS && band->ins[i].width; i++) {
		LayoutInsert in = band->ins[i];
		if (in.at <= x || in.at >= x + w) continue;
		if (in.src < x || in.src + in.srcW > x + w) return 0;
		in.at -= x;
		in.src -= x;
		d.bands[0].ins[n++] = in;
	}
	if (!n) return 0;
	*out = d;
	return 1;
}

// Columns inserted before column x (for a left edge) or before x itself is reached (for an
// exclusive right edge, so a view ending at an insertion point does not grow).
static int layout_shift_left(const LayoutBand *band, int x)
{
	int shift = 0;
	for (int i = 0; i < LAYOUT_MAX_INSERTS && band->ins[i].width; i++)
		if (band->ins[i].at <= x) shift += band->ins[i].width;
	return shift;
}

static int layout_shift_right(const LayoutBand *band, int x)
{
	int shift = 0;
	for (int i = 0; i < LAYOUT_MAX_INSERTS && band->ins[i].width; i++)
		if (band->ins[i].at < x) shift += band->ins[i].width;
	return shift;
}

// Where a view of the 800-wide layout goes: new left edge and width.
static void layout_place(const ScreenLayout *s, int x, int y, int w, int h, int *nx, int *nw)
{
	const LayoutBand *band = layout_band(s, y + h / 2);
	int left = layout_shift_left(band, x), right = layout_shift_right(band, x + w);
	*nx = x + left;
	*nw = w + right - left;
}

// The source column of every column of one band of the widened picture.
static void layout_band_map(const ScreenLayout *s, const LayoutBand *band, short *map)
{
	int out = 0, from = 0;
	unsigned seed = 0x9e3779b9u ^ (unsigned)band->top;
	for (int i = 0; i <= LAYOUT_MAX_INSERTS; i++) {
		const LayoutInsert *in = i < LAYOUT_MAX_INSERTS && band->ins[i].width ? &band->ins[i] : 0;
		int upTo = in ? (in->mode == 'S' ? in->src : in->at) : s->width;
		while (from < upTo) map[out++] = (short)from++;
		if (!in) break;
		if (in->mode == 'S') {
			int total = in->width + in->srcW;
			for (int k = 0; k < total; k++) map[out++] = (short)(in->src + k * in->srcW / total);
			from = in->src + in->srcW;
		} else if (in->mode == 'T' || in->mode == 'U') {
			// Phase matched to the column before the insertion, so the ornament runs on unbroken
			// into the repeat, or to the column after it.
			int first = in->mode == 'T' ? in->at : in->at - in->width;
			int phase = ((first - in->src) % in->srcW + in->srcW) % in->srcW;
			for (int k = 0; k < in->width; k++) map[out++] = (short)(in->src + (phase + k) % in->srcW);
		} else if (in->mode == 'M') {
			// A stretch ending at the insertion runs back from it first, one starting there runs
			// forward first; either way the seams meet their own mirror image. With a width of
			// twice the stretch both ends join the original artwork unbroken.
			int forwardFirst = in->src >= in->at;
			for (int k = 0; k < in->width; k++) {
				int pass = k / in->srcW, step = k % in->srcW;
				int forward = (pass % 2 == 0) == forwardFirst;
				map[out++] = (short)(forward ? in->src + step : in->src + in->srcW - 1 - step);
			}
		} else {
			for (int k = 0; k < in->width;) {
				seed = seed * 1103515245u + 12345u;
				int run = 6 + (int)((seed >> 16) % 11);
				seed = seed * 1103515245u + 12345u;
				int start = in->srcW > run ? (int)((seed >> 16) % (unsigned)(in->srcW - run)) : 0;
				for (int j = 0; j < run && k < in->width; j++, k++) map[out++] = (short)(in->src + (start + j) % in->srcW);
			}
		}
	}
}

static inline const ScreenLayout *layout_by_name(const char *name)
{
	for (int i = 0; i < (int)(sizeof g_screenLayouts / sizeof g_screenLayouts[0]); i++) {
		const char *a = g_screenLayouts[i].name, *b = name;
		while (*a && *a == *b) a++, b++;
		if (!*a && !*b) return &g_screenLayouts[i];
	}
	return 0;
}
