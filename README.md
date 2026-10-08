# Imperialism II HD

A fan patch that makes *Imperialism II: Age of Exploration* (1999) fill a modern display, plus the
reverse-engineering tooling used to build it.

The game is hard-wired to 800x600. Out of the box it either switches your monitor to that
resolution or draws a small box in the middle of the desktop. With this patch the desktop stays at
its native resolution and the whole game is scaled to the largest 4:3 size that fits, centred with
black bars at the sides.

You need your own copy of the game. Nothing from the game is included here, and the game's files
are never modified.

## Status

Developed and tested against the GOG release (v1.03 executable) on Windows 11.

Working:

- main menu, map, load/save screens
- pop-ups and the pause menu
- intro movies
- text entry fields
- mouse input and the game's custom cursors

Not yet checked: city, trade, diplomacy and tactical battle screens, and multiplayer.

Known limits:

- The picture is still 4:3. Using the full width of a widescreen display is the next stage.
- Scaling is done on the CPU by GDI. On very large displays, scrolling may be slower than stock.

## Install

1. Build `ddraw.dll` (see below).
2. Copy it into the game folder, next to `Imperialism II.exe`.
3. Start the game as usual.

To uninstall, delete `ddraw.dll` from the game folder.

## Settings

Optional. Create `imp2hd.ini` next to the exe:

```ini
[Display]
Enabled=1
Smooth=1
IntegerScale=0
```

| Key | Default | Effect |
|---|---|---|
| `Enabled` | 1 | 0 turns the patch off without removing the DLL |
| `Smooth` | 1 | 0 uses faster, blockier nearest-neighbour scaling |
| `IntegerScale` | 0 | 1 snaps to a whole-number scale for the sharpest pixels |

`imp2hd.log` is rewritten in the game folder on each launch and records what the patch did.

## Build

The DLL must be 32-bit. It is built with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw):

```powershell
.\hd\build.ps1            # builds hd\ddraw.dll
.\hd\build.ps1 -Install   # also copies it into the game folder
```

`build.ps1` has the compiler path hard-coded near the top; change it to match your install. The
`-Install` switch assumes this repository sits in a folder directly inside the game folder.

## How it works

`ddraw.dll` is a proxy: the game loads it in place of the system DirectDraw library, and every
DirectDraw call is forwarded to the real one. While loading, it does three things in memory:

- **Skips the mode switch.** One function in the game sets the monitor to 800x600x16. It is
  patched to report success without touching the display. The patch is only applied if the
  executable matches the expected v1.03 build.
- **Scales drawing.** The game paints each frame into an 800x600 offscreen bitmap and copies it to
  its window. The window is enlarged, and every device context for it is given a GDI mapping from
  800x600 units to its real size, so the game's own drawing calls come out enlarged.
- **Scales everything else to match.** Mouse coordinates, invalidation rectangles and client-size
  queries are converted between the two sizes. Pop-up windows, the movie player and text fields
  are repositioned and resized, text fields get an enlarged font, and the game's 1999-style masked
  cursors are rebuilt as alpha cursors.

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

`ghidra_scripts/Probe.java` is a helper for decompiling an address range or listing references to
an address when a function did not get a name.

## Layout

| Path | Contents |
|---|---|
| `hd/imp2hd.c`, `hd/imp2hd.def` | the scaling DLL |
| `hd/build.ps1` | build and install |
| `hd/shot.ps1`, `hd/cursorshot.ps1` | launch the game and capture screenshots for testing |
| `tools/` | linker-map conversion and name matching |
| `ghidra_scripts/` | Ghidra headless scripts |

## Legal

*Imperialism II* is a commercial game that is still on sale; its code, artwork and other assets
belong to their owners. This project is not affiliated with or endorsed by them.
