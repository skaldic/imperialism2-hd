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
//   'G' takes the columns from a generated picture of the whole widened screen instead (see
//       generated), falling back to 'M' of src and srcW when there is none
//   'Q' mirror-tiles a patch of plain texture in both directions: columns src..src+srcW, rows
//       srcY..srcY+srcH (for paper with nothing plain beside the insertion in the same rows)
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

typedef struct { short at, width, src, srcW; char mode; short srcY, srcH; } LayoutInsert;
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
	// The widened screen painted out by an image model, at 1x: assets/wide/<generated>.png at the
	// widened size, and its upscale assets/x2/_wide/<generated>.png, for 'G' insertions.
	short generated;

} ScreenLayout;

#define LID(a, b, c, d) (((unsigned long)(a) << 24) | ((unsigned long)(b) << 16) | ((unsigned long)(c) << 8) | (unsigned long)(d))

// The diplomacy screens' bands. In the chart the map of nations moves 125, not 128: the patch
// moves its drawing by whole map columns of 5 pixels (DIPLOMACY_MAP_SHIFT in imp2hd.c).
#define DIPLOMACY_BANDS { \
		{ 0, 55, { { 135, 128, 135, 20, 'S' }, { 667, 128, 647, 20, 'S' } } }, \
		/* the frame's floral border along the top */ \
		{ 55, 96, { { 135, 128, 140, 40, 'R' }, { 667, 128, 600, 40, 'R' } } }, \
		/* the panel's interior (to its right end at 667, so the colour key, which ends at 666, keeps its size) */ \
		{ 96, 216, { { 135, 128, 135, 64, 'M' }, { 667, 128, 603, 64, 'M' } } }, \
		/* the frame's lower edge and the gold rope under it */ \
		{ 216, 245, { { 135, 128, 160, 8, 'T' }, { 667, 128, 600, 8, 'T' } } }, \
		{ 245, 600, { { 100, 125, 80, 20, 'S' }, { 705, 131, 705, 20, 'S' } } }, \
	}
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
		"diplomacy", "TDiplomacyMapView", LID('m', 'a', 'i', 'n'), LID('h', 'e', 'r', 'e'), 800, 600,
		DIPLOMACY_BANDS,
		// The Information panel is a picture filled to its edges (its colour key). The map of
		// nations is drawn at fixed positions, moved by the patch (DIPLOMACY_MAP_SHIFT).
		0, 1,
	},
	// The same map of nations on the screens for offers made to you and for the final vote.
	{ "diplomatic offers", "TDiplomacyOfferView", LID('m', 'a', 'i', 'n'), LID('h', 'e', 'r', 'e'), 800, 600,
		DIPLOMACY_BANDS, 0, 1 },
	{ "final vote", "TDiplomacyFinalView", LID('m', 'a', 'i', 'n'), LID('h', 'e', 'r', 'e'), 800, 600,
		DIPLOMACY_BANDS, 0, 1 },
	// And on the battle report, which is built on the diplomacy screen (with the red cloth).
	{ "battle report", "TBattleReportView", LID('m', 'a', 'i', 'n'), LID('h', 'e', 'r', 'e'), 800, 600,
		DIPLOMACY_BANDS, 0, 1 },
	{
		// The same screen on its other background, a red cloth (the same places, other fill).
		"diplomacy (cloth)", "TDiplomacyMapView", LID('m', 'a', 'i', 'n'), 0, 800, 600, {
			{ 0, 55, { { 135, 128, 135, 20, 'S' }, { 667, 128, 647, 20, 'S' } } },
			{ 55, 96, { { 135, 128, 135, 20, 'S' }, { 667, 128, 647, 20, 'S' } } },
			{ 96, 216, { { 135, 128, 135, 20, 'S' }, { 667, 128, 647, 20, 'S' } } },
			{ 216, 245, { { 135, 128, 160, 11, 'T' }, { 667, 128, 600, 11, 'T' } } },
			{ 245, 600, { { 100, 125, 80, 20, 'S' }, { 705, 131, 705, 20, 'S' } } },
		},
		4100,
	},
	{
		// Loading and saving: a chest of scrolls (the save slots) in a room. The room is widened
		// either side of the chest: the red curtain on the left, the green wallpaper and the wood
		// panelling on the right. The chest's gilded base runs the full width, so it is lengthened
		// either side of its middle, in its ornament's repeat.
		"load and save", "TLoadSavePicture", LID('m', 'a', 'i', 'n'), LID('s', 'l', 't', '0'), 800, 600, {
			{ 0, 180, { { 25, 128, 0, 25, 'S' }, { 790, 128, 758, 32, 'M' } } },
			// On the right, everything is added at the picture's edge, from the strip beside
			// it: the panelling mirrored (its grooves run level), the carpet mirrored too, from the strip
			// from right of the chest's slanting side, which comes close to the edge lower down.
			{ 180, 400, { { 25, 128, 0, 25, 'S' }, { 800, 128, 770, 30, 'M' } } },
			{ 400, 460, { { 25, 128, 0, 25, 'S' }, { 800, 128, 776, 24, 'M' } } },
			{ 460, 485, { { 25, 128, 0, 25, 'S' }, { 800, 128, 791, 9, 'M' } } },
			{ 485, 500, { { 25, 128, 0, 25, 'S' }, { 800, 128, 797, 3, 'S' } } },
			{ 500, 575, { { 200, 128, 100, 101, 'T' }, { 600, 128, 600, 101, 'T' } } },
			{ 575, 600, { { 200, 128, 190, 10, 'S' }, { 600, 128, 600, 10, 'S' } } },
		},
	},
	{
		// The main menu: one painting of a room, with no plain stretch to lengthen. It is kept
		// whole and centred, and the room is painted out either side by an image model
		// (tools\outpaint.ps1).
		"main menu", "TGameSetupPicture", LID('m', 'a', 'i', 'n'), LID('l', 'o', 'a', 'd'), 800, 600, {
			{ 0, 600, { { 0, 128, 0, 32, 'G' }, { 800, 128, 768, 32, 'G' } } },
		},
		0, 0, 4502,
	},
	// (No layout for the technology screen: both its panels are filled edge to edge by controls
	// and its frame is too thin to take the width, so it stays centred.)
	{
		// The industry screen's page: graph paper with ruled rows, one picture per tab. Each is
		// lengthened inside its rows, just after their left ornaments and just before their right
		// ones, by mirroring a plain stretch of the same rows, so the ruling and the grid carry on.
		// Production (and the default for any other artwork of the page).
		"industry page", "TBackgroundPicture", LID('p', 'a', 'g', 'e'), 0, 595, 361, {
			{ 0, 361, { { 33, 128, 33, 32, 'M' }, { 558, 128, 526, 32, 'M' } } },
		},
	},
	{
		// Workers: the same above its lower boxes; beside those, paper from between the top boxes.
		"industry page (workers)", "TBackgroundPicture", LID('p', 'a', 'g', 'e'), 0, 595, 361, {
			{ 0, 255, { { 33, 128, 33, 32, 'M' }, { 560, 128, 556, 4, 'M' } } },
			{ 255, 361, { { 33, 128, 40, 64, 'Q', 96, 28 }, { 560, 128, 40, 64, 'Q', 96, 28 } } },
		},
		9198,
	},
	{
		// Shipyard: panels instead of rows. Inside the left panel, left of its oval, and inside
		// the right panels after their last pictures.
		"industry page (shipyard)", "TBackgroundPicture", LID('p', 'a', 'g', 'e'), 0, 595, 361, {
			// Grid paper from inside the left panel where nothing plain runs along the rows; the
			// left panel's top and bottom frames are carried on with it.
			{ 0, 34, { { 22, 128, 60, 128, 'Q', 150, 190 }, { 582, 128, 60, 128, 'Q', 150, 190 } } },
			{ 34, 46, { { 22, 128, 22, 8, 'M' }, { 582, 128, 60, 128, 'Q', 150, 190 } } },
			{ 46, 140, { { 22, 128, 60, 128, 'Q', 150, 190 }, { 582, 128, 60, 128, 'Q', 150, 190 } } },
			{ 140, 350, { { 22, 128, 60, 128, 'Q', 150, 190 }, { 582, 128, 60, 128, 'Q', 150, 190 } } },
			{ 350, 361, { { 22, 128, 22, 8, 'M' }, { 582, 128, 60, 128, 'Q', 150, 190 } } },
		},
		9200,
	},};

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
			if (in->mode == 'Q' && (in->srcH <= 0 || in->srcY < 0 || in->srcY + in->srcH > s->height)) return 0;
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

// Mirror tiling: 0, 1, .. n-1, n-1, .. 0, 0, 1, ..
static int layout_mirror(int v, int n)
{
	int p = 2 * n;
	v %= p;
	if (v < 0) v += p;
	return v < n ? v : p - 1 - v;
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
		} else if (in->mode == 'Q') {
			for (int k = 0; k < in->width; k++) map[out++] = -3; // drawn by the patch fill
		} else if (in->mode == 'G' && s->generated) {
			for (int k = 0; k < in->width; k++) map[out++] = -1; // a generated column (drawn by the patch)
		} else if (in->mode == 'M' || in->mode == 'G') {
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
