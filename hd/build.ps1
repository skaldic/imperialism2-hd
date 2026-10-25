# Builds the HD scaling DLL. Pass -Install to also copy it next to the game as ddraw.dll.
# Needs llvm-mingw's 32-bit clang (i686-w64-mingw32-clang): on PATH, or in an llvm-mingw-*
# folder of the external tools folder (tools\tools_dir.ps1).
param([switch]$Install)

$cc = (Get-Command i686-w64-mingw32-clang.exe -ErrorAction SilentlyContinue).Source
if (-not $cc) {
    $tools = & "$PSScriptRoot\..\tools\tools_dir.ps1"
    $cc = Get-ChildItem -Path $tools -Filter i686-w64-mingw32-clang.exe -Recurse -Depth 2 -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $cc) { throw "i686-w64-mingw32-clang.exe not found: put llvm-mingw on PATH or in the tools folder (tools\tools_dir.ps1)" }
$out = "$PSScriptRoot\ddraw.dll"

& $cc -O2 -Wall -shared -static -o $out "$PSScriptRoot\imp2hd.c" "$PSScriptRoot\imp2hd.def" -luser32 -lgdi32 -lgdiplus -lmsimg32
if ($LASTEXITCODE -ne 0) { throw "build failed" }
"built $out ($((Get-Item $out).Length) bytes)"

if ($Install) {
    Copy-Item $out "$PSScriptRoot\..\..\ddraw.dll" -Force -ErrorAction Stop
    "installed to game folder"
}
