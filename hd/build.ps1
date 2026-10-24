# Builds the HD scaling DLL. Pass -Install to also copy it next to the game as ddraw.dll.
param([switch]$Install)

$cc = "D:\Tools\llvm-mingw-20261006-msvcrt-x86_64\bin\i686-w64-mingw32-clang.exe"
$out = "$PSScriptRoot\ddraw.dll"

& $cc -O2 -Wall -shared -static -o $out "$PSScriptRoot\imp2hd.c" "$PSScriptRoot\imp2hd.def" -luser32 -lgdi32 -lgdiplus
if ($LASTEXITCODE -ne 0) { throw "build failed" }
"built $out ($((Get-Item $out).Length) bytes)"

if ($Install) {
    Copy-Item $out "$PSScriptRoot\..\..\ddraw.dll" -Force -ErrorAction Stop
    "installed to game folder"
}
