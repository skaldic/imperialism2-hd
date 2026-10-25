# Imperialism II HD

An unofficial fan patch that makes *Imperialism II: Age of Exploration* (1999) fill a modern
display: the game at your screen's resolution, wide screens, HD art and sharp text. It also holds
the reverse-engineering tooling used to build it.

The game is hard-wired to 800x600. Out of the box it either switches your monitor to that
resolution or draws a small box in the middle of the desktop. With this patch the desktop stays at
its native resolution and the game is scaled to fit; the map, battle and most other screens are
widened to 1056x600, close to 16:9, and the art is replaced by 2x versions.

## For players

Download the latest archive from [Releases](https://github.com/skaldic/imperialism2-hd/releases)
and unzip it into the game's folder, so that `ddraw.dll` and `imp2hd.ini` sit next to
`Imperialism II.exe`. The included `imp2hd\README.txt` has the details. To uninstall, delete
`ddraw.dll`, `imp2hd.ini` and the `imp2hd` folder. The game's own files are never modified.

You need your own copy of the game: the GOG release (v1.03 executable) on Windows 10 or 11 is what
the patch is made and tested for. Other builds load the patch but it leaves them unchanged.

What it does:

| Feature | Setting | Notes |
|---|---|---|
| Scaling to the display | always on | Every screen |
| Wide map, battlefield and screens (1056x600) | `Wide=1` | Map, battles, transport, industry, trade, diplomacy and its offers, load/save, deals, technology, new game, preferences, high scores, credits. Screens that are one painting (the main menu) are painted out at the sides |
| HD art | `HD=1` | Upscaled terrain, units, towns, icons and screens |
| HD map units | `HDMapSprites=1` | Each unit and town drawn from its own HD picture, shadows smoothed |
| Sharp text | `SharpText=1` | Text drawn again at full resolution |
| Main menu at 4x | `HDNative=1` | Drawn at the display's own resolution |

Known limits:

- Map squares are captured and upscaled as they are met in play, so the released art covers the
  combinations seen while it was made. On other maps a new mix of coast, river and road shows
  smoothed instead of HD.
- Some dialogs and the city screens stay 800 wide, centred, with painted sides.
- Multiplayer has not been tried.

Problems: open an [issue](https://github.com/skaldic/imperialism2-hd/issues) and attach
`imp2hd.log` from the game folder (rewritten on each launch).

## Settings

`imp2hd.ini` next to the exe, section `[Display]`:

| Key | Default | Effect |
|---|---|---|
| `Enabled` | 1 | 0 turns the patch off without removing the DLL |
| `Wide` | 0 | 1 widens the screens to 1056x600 (the release ini sets it) |
| `HD` | 0 | 1 uses the HD art (the release ini sets it) |
| `Assets` | `imp2hd` in the game folder, else `decomp\assets` | Where the HD art is |
| `SharpText` | 1 | Draw text again at 2x in a doubled font (fonts that do not scale evenly keep their 1x text) |
| `HDMapSprites` | 1 | Units, towns and other map pictures from their HD art |
| `HDNative` | 1 | Painted-out full-screen pictures (the main menu) at the display's resolution from their 4x art |
| `Smooth` | 1 | 0 uses blocky nearest-neighbour scaling |
| `FastScale` | 1 | 0 uses GDI's own smooth scaling, which is much slower on large displays |
| `IntegerScale` | 0 | 1 snaps to a whole-number scale |
| `MarginArt` | 3 | Beside centred screens: 0 black, 1 blurred edge, 2 tiled game texture, 3 the screen's background continued |
| `HDCaptureTiles` | 0 | 1 saves new map squares for upscaling as they are met (needs the tools below) |
| `HDAutoUpscale` | 0 | 1 upscales captured squares when the game closes (needs the tools below) |

`[Debug]` keys (`DumpViews`, `TraceCopies`, `Timing`, `WideSteps`, `LogCrashes`) add diagnostics to
`imp2hd.log`.

## Contributing

Changes go through pull requests to `main`; each needs the owner's approval before it is merged.
Open an issue first for anything large.

### Tools

| Tool | Used for |
|---|---|
| [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) | building the 32-bit DLL |
| [Real-ESRGAN ncnn-vulkan](https://github.com/xinntao/Real-ESRGAN/releases) | upscaling the art |
| [stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp) with an SDXL inpainting model and the SDXL fp16 VAE | painting out the sides of one-painting screens |
| [Ghidra](https://github.com/NationalSecurityAgency/ghidra) and a JDK | the decompilation tooling (optional) |

Put them in one folder and set the `IMP2HD_TOOLS` environment variable to it, or put them in
`tools\external`. The scripts look for `realesrgan\realesrgan-ncnn-vulkan.exe`,
`sdcpp\bin\sd-cli.exe`, `sdcpp\models\sdxl_inpaint.safetensors`,
`sdcpp\models\sdxl_vae_fp16_fix.safetensors` and `llvm-mingw-*\bin` there
(`tools\tools_dir.ps1`); the compiler can also be on `PATH`.

The repository is expected in a folder directly inside the game folder (`<game>\decomp`), so that
`build.ps1 -Install` and the art paths find the game.

### Build

```powershell
.\hd\build.ps1            # builds hd\ddraw.dll
.\hd\build.ps1 -Install   # also copies it into the game folder
```

### HD art

The art is made from your own copy of the game and is not part of this repository.

```powershell
.\tools\extract_rsrc.ps1   # Data\*.rsrc -> assets\orig\<archive>\<id>.png
.\tools\upscale_all.ps1    # assets\orig -> assets\x2 (and the HD pack), a few minutes on a recent GPU
```

`upscale_all.ps1` runs each picture through Real-ESRGAN at 4x and reduces it to 2x. Along real
outlines the model's output is kept; elsewhere most of it is replaced by a plain enlargement, so
dithered textures are not redrawn as patterns that were never there (`-Grain`, `-EdgeLow`,
`-EdgeHigh`, `-Model`). Pictures with the magenta transparency key have the key area filled from
the sprite before upscaling and stamped back afterwards. `-Only <archive>` limits a run;
`-NewOnly` skips pictures that are already up to date; `-SpritesOnly` does only pictures with
transparency.

Map squares are composed by the game at run time, so there is nothing to upscale in advance. With
`HDCaptureTiles=1` the patch saves every new square it meets to `assets\orig\_tiles`;
`upscale_all.ps1 -Only _tiles` (or `HDAutoUpscale=1`, when the game closes) makes them HD.
`grain_tiles.ps1` then puts the original's fine grain and soft grass patches back into terrain, and
`build_pack.ps1` packs every square and small picture into `assets\x2\hd.pack`, which the game
reads instead of thousands of PNG files.

Screens that are one painting (the main menu, the sea chart beside the map of nations) are painted
out by an image model: `outpaint.ps1` and `inpaint_strips.ps1` (with `match_margins.ps1` and
`refine_region.ps1` to clean up) write `assets\wide\<id>.png` and its 2x and 4x upscales.

### Release

```powershell
.\tools\make_release.ps1 -Version 1.0.0   # dist\ImperialismII-HD-1.0.0.zip
```

It builds the DLL and packs it with a default ini, the HD art (the pack, the large pictures, and the
finished painted-out screens), `INSTALL.txt`, the license and the notice.

### Testing

`hd\drive.ps1` starts the game and plays back clicks, keys and screenshots, for example
`.\hd\drive.ps1 -GameW 1056 wait:8 click:278,450 wait:4 shot:load`. It only clicks or types while
the game is the window in front. `hd\layouttool.c` previews a wide layout offline from the
extracted art and a view dump (`tools\views_from_log.ps1`).

## How it works

`ddraw.dll` is a proxy: the game loads it in place of the system DirectDraw library, and every
DirectDraw call is forwarded to the real one. Everything else is done in memory while it loads; all
addresses are for the v1.03 executable, and patches are only applied if the build matches. All of
it is in `hd/imp2hd.c`.

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
counts, cache bitmaps and visible-column constants are raised to match.

**Wide battlefield.** The battlefield picture is about twice as wide as the battle view and the
view scrolls across it, with limits taken from the view's own width, so the view is simply made
wider. Its gilt corner pieces are drawn at fixed positions, which are moved, and the background is
lengthened like the other screens'.

**Wide screens.** Screens drawn on a background picture get a table-driven layout (`hd/layouts.h`).
In each horizontal band of the picture, columns are inserted at chosen places and filled from plain
artwork nearby: repeated, mirrored, scrambled in short runs, smoothly stretched, or taken from a
painted-out picture. Every view is moved by the insertions to its left and widened by any under it,
and the layout is re-applied whenever the game moves views (tabs and panels). A few positions are
fixed in the game's drawing code and are patched to match: the trade screen's goods icons and the
diplomacy screens' map of nations.

**HD art.** The game keeps drawing at 1x. Its picture-copy routine is intercepted; when it copies a
picture for which an HD version exists, the same rectangle is also drawn at 2x into the 2x frame,
and the 1x pixels written are remembered. When the frame is shown, pixels that still hold the
remembered value come from the HD art and everything else is smoothed from the 1x frame. Pictures
are matched to the game's archives by hashing their pixels; map units and other sprites by the
number the game loaded them by (its resource manager and its sprite buffers are watched), so no
sprite is guessed from its pixels. The map is assembled in a second buffer before it reaches the
frame; that buffer gets the same treatment and its HD blocks travel with it.

**Sharp text.** Every text call is drawn a second time into the 2x frame in a font of twice the
size, with each character at twice its 1x position, and kept only where the 1x text landed.

## Decompilation tooling

The game ships with a linker map from an earlier build, which names most of its functions. These
scripts carry those names over to the shipped executable and produce readable pseudo-C, one file
per original source file. The output is for study only and is deliberately not part of this
repository; generate it from your own copy.

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
placed this way; treat names as strong hints, not facts.

## License

The code and tools in this repository are under the
[PolyForm Noncommercial License 1.0.0](LICENSE.md): you may use, change and share them for any
non-commercial purpose; nobody may use them commercially.

*Imperialism II* is a commercial game; its code, artwork and other assets belong to their owners,
and this project is not affiliated with or endorsed by them. The release archives contain artwork
made from the game's own pictures, which the license does not cover; see [NOTICE.md](NOTICE.md).
