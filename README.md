# Imperialism II HD

A fan patch that makes *Imperialism II: Age of Exploration* (1999) fill a modern display, plus the
reverse-engineering tooling used to build it.

The game is hard-wired to 800x600. Out of the box it either switches your monitor to that
resolution or draws a small box in the middle of the desktop. With this patch the desktop stays at
its native resolution and the game is scaled to fit. Optionally the map, battle and main management screens are widened to use a
16:9 display, and the artwork is replaced by 2x upscaled versions.

You need your own copy of the game. Nothing from the game is included here, and the game's files
are never modified.

## Status

Developed and tested against the GOG release (v1.03 executable) on Windows 11.

| Feature | Setting | State |
|---|---|---|
| Scaling to the display | always on | Works on every screen tried |
| Wide map (13 tile columns instead of 9) | `Wide=1` | Works; land map and zoomed-out overview |
| Wide battlefield (1031 instead of 775 pixels) | `Wide=1` | Works on the battles tried |
| Wide transport, industry, trade and diplomacy screens | `Wide=1` | Works on the screens tried |
| HD artwork | `HD=1` | Works; quality varies, see below |

Not yet checked: multiplayer, with any of the options on.

Known limits:

- The technology screen, city and battle-report screens, menus and dialogs are not widened: they stay 800 wide and is shown centred, with
  its own background continued into the margins.
- HD art is an automatic upscale. Line art and icons come out well; fine textures and very small
  figures less so.
- The zoomed-out overview is not HD.

## Install

1. Build `ddraw.dll` (see below).
2. Copy it into the game folder, next to `Imperialism II.exe`.
3. Start the game as usual.

To uninstall, delete `ddraw.dll` from the game folder.

## Settings

Optional. Create `imp2hd.ini` next to the exe:

```ini
[Display]
Wide=1
HD=1
```

| Key | Default | Effect |
|---|---|---|
| `Enabled` | 1 | 0 turns the patch off without removing the DLL |
| `Smooth` | 1 | 0 uses blocky nearest-neighbour scaling |
| `FastScale` | 1 | 0 uses GDI's own smooth scaling, which is much slower on large displays |
| `IntegerScale` | 0 | 1 snaps to a whole-number scale |
| `Wide` | 0 | 1 widens the map, battle and management screens to 1056x600 |
| `MarginArt` | 3 | Beside centred screens in wide mode: 0 black, 1 blurred edge, 2 tiled game texture, 3 the screen's background continued |
| `HD` | 0 | 1 uses upscaled artwork (needs the assets, see below) |
| `HDAssets` | `decomp\assets\x2` in the game folder | Where the upscaled artwork is |
| `HDCaptureTiles` | 1 | Save new map tiles for upscaling as they are met |
| `HDAutoUpscale` | 1 | Upscale newly captured tiles when the game closes |

`imp2hd.log` is rewritten in the game folder on each launch and records what the patch did.
`[Debug]` keys (`DumpViews`, `TraceCopies`, `Timing`, `WideSteps`) add diagnostics to it.

## Build

The DLL must be 32-bit. It is built with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw):

```powershell
.\hd\build.ps1            # builds hd\ddraw.dll
.\hd\build.ps1 -Install   # also copies it into the game folder
```

`build.ps1` has the compiler path hard-coded near the top; change it to match your install. The
`-Install` switch assumes this repository sits in a folder directly inside the game folder.

## HD artwork

The upscaled art is made from your own copy of the game and is not part of this repository.
It needs [Real-ESRGAN ncnn-vulkan](https://github.com/xinntao/Real-ESRGAN/releases); the scripts
expect it in `D:\Tools\realesrgan` unless told otherwise with `-Upscaler`.

```powershell
.\tools\extract_rsrc.ps1   # Data\*.rsrc -> assets\orig\<archive>\<id>.png
.\tools\upscale_all.ps1    # assets\orig -> assets\x2, a few minutes on a recent GPU
```

`upscale_all.ps1` runs each picture through the `realesrgan-x4plus` model and reduces the result
to 2x. Along real outlines the model's output is kept; elsewhere most of it is replaced by a plain
enlargement, so dithered textures are not redrawn as patterns that were never there (`-Grain`,
`-EdgeLow`, `-EdgeHigh`). Pictures with the magenta transparency key have the key area filled from
the sprite before upscaling and stamped back afterwards with a rounded mask. `-Only <archive>`
limits a run; `-NewOnly` skips pictures that are already up to date.

Map tiles are different: the game composes each one at run time, so there is no picture to upscale
in advance. With `HD=1` the patch saves every new tile it meets to `assets\orig\_tiles` and, when
the game closes, upscales them into `assets\x2\_tiles`. A part of the map therefore turns HD the
second time you visit it.

## How it works

`ddraw.dll` is a proxy: the game loads it in place of the system DirectDraw library, and every
DirectDraw call is forwarded to the real one. Everything else is done in memory while it loads; all
addresses are for the v1.03 executable, and patches are only applied if the build matches.

**Scaling.** One function in the game sets the monitor to 800x600x16; it is patched to report
success without touching the display. The game paints each frame into an offscreen bitmap and
copies it to its window. The window is enlarged and the copy is intercepted: the frame is doubled
with interpolation into a 2x frame and that is stretched to the window. Mouse coordinates,
invalidation rectangles and client-size queries are converted between the two sizes. Pop-up
windows, the movie player and text fields are repositioned and resized, text fields get an
enlarged font, and the game's masked cursors are rebuilt as alpha cursors.

**Wide map.** The game's emulated Mac screen, its frame buffer and its window-centring are widened
from 800 to 1056. Screens are trees of view objects built in code; when the map screen appears,
its views are moved and resized. The map view keeps a cache of rendered tiles with bookkeeping in
a fixed 110-slot array inside the view object, sized for a 9-column view. That array is relocated
to the end of an enlarged object by repointing every instruction that indexes it, and the slot
counts, cache bitmaps and visible-column constants are raised to match. The zoomed-out overview
has no cache, only loop bounds and limits to change. The map screen's background picture is drawn
by a replacement routine that moves the side panel's art to the new edge and fills the gap with a
repeated slice of the border. Other screens are left at 800 and shifted when shown.

**Wide battlefield.** The battlefield picture is about twice as wide as the battle view and the
view scrolls across it, with limits taken from the view's own width, so the view is simply made
wider. Its gilt corner pieces are drawn at fixed positions, which are moved. The background is
lengthened like the map screen's: the title bar in its middle and the toolbar panel in two plain
stretches, with the toolbar's buttons spread out to match. On a battle too small to fill the wider
view, the view is scrolled onto the picture's unused terrain beside the field rather than past the
picture's edge.

**Wide management screens.** Screens drawn on a single background picture get a table-driven
layout (`hd/layouts.h`). In each horizontal band of the picture, columns are inserted at chosen
places and filled from plain artwork nearby: repeated, mirrored, scrambled in short runs, or
smoothly stretched. Every view is moved by the insertions to its left and widened by any under it.
The game moves views later (tabs and panels are swapped by moving them on and off screen), so the
layout is re-applied whenever it does. A few positions are fixed in the game's drawing code and are
patched to match: the trade screen's goods icons and the diplomacy screens' map of nations. That
map's country shapes and drawing code are generated once at a fixed screen position, so the
generator is shown the map view where the layout puts it. `hd/layouttool.c` previews a layout
offline from the extracted artwork and a view dump (`tools/views_from_log.ps1`).

**HD artwork.** The game keeps drawing at 1x. Its picture-copy routine is intercepted; when it
copies a picture for which an upscaled version exists, the same rectangle is also copied at 2x
into the 2x frame, and the 1x pixels written are remembered. When the frame is shown, pixels that
still hold the remembered value come from the HD art and everything else is smoothed from the 1x
frame. Full-colour pictures are matched to files by hashing their pixel data against the game's
archives; 8-bit pictures, whose palette indices the game remaps on load, by similarity. The map is
assembled in a second buffer before it reaches the frame, so that buffer gets the same treatment
and its HD blocks travel with it.

All of it is in `hd/imp2hd.c`.

## Decompilation tooling

The game ships with a linker map from an earlier build, which names most of its functions. These
scripts carry those names over to the shipped executable and produce readable pseudo-C, one file
per original source file. The output is for study only and is deliberately not part of this
repository; generate it from your own copy.

Requires [Ghidra](https://github.com/NationalSecurityAgency/ghidra) and a JDK. Paths below assume
the repository is in a folder inside the game folder.

```powershell
# 1. Linker map -> symbols_map_jan1999.csv
.\tools\map_to_csv.ps1

# 2. Import and analyse the exe, export the functions Ghidra found -> functions_v103.csv
analyzeHeadless.bat .\ghidra_proj Imp2 -import "..\Imperialism II.exe" `
    -scriptPath .\ghidra_scripts -postScript ExportFunctions.java "$PWD\functions_v103.csv"

# 3. Match map names to the shipped exe -> names_v103.csv
.\tools\match_names.ps1

# 4. Apply the names and decompile -> src\*.c
analyzeHeadless.bat .\ghidra_proj Imp2 -process "Imperialism II.exe" `
    -scriptPath .\ghidra_scripts `
    -postScript ApplyNames.java "$PWD\names_v103.csv" `
    -postScript DecompileByObj.java "$PWD\names_v103.csv" "$PWD\src"
```

The map is from January 1999 and the executable from June 1999, so addresses do not line up.
`match_names.ps1` relies on both builds sharing link order: it finds the sequence of address
shifts that lands the most map functions on real function starts. About 93% of game functions are
placed this way, and a spot check against class names embedded in the executable found roughly 95%
of those correct. Treat names as strong hints, not facts.

`Probe.java` decompiles an address range or lists references to an address, for functions that
did not get a name. `FindInstructions.java` lists instructions matching a regular expression,
which is how patch sites were enumerated.

## Layout

| Path | Contents |
|---|---|
| `hd/imp2hd.c`, `hd/imp2hd.def` | the DLL |
| `hd/build.ps1` | build and install |
| `hd/drive.ps1` | launch the game and play back clicks, keys and screenshots |
| `hd/widetest.ps1`, `hd/menutest.ps1`, `hd/shot.ps1`, `hd/cursorshot.ps1` | test runs built on it |
| `tools/extract_rsrc.ps1`, `tools/upscale_all.ps1`, `tools/upscale_compare.ps1` | artwork pipeline |
| `tools/map_to_csv.ps1`, `tools/match_names.ps1` | linker-map conversion and name matching |
| `ghidra_scripts/` | Ghidra headless scripts |

## Legal

*Imperialism II* is a commercial game that is still on sale; its code, artwork and other assets
belong to their owners. This project is not affiliated with or endorsed by them.
