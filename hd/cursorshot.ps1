# Launches the game, parks the mouse at a point given in 800x600 game coordinates, and saves a
# full-resolution crop of the screen around it. Used to inspect drawing glitches near the cursor.
param(
    [int]$GameX = 400,
    [int]$GameY = 250,
    [int]$Wait = 9,
    [int]$Crop = 900,
    [string]$Out = "$PSScriptRoot\shots\cursor.png"
)

Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type -Namespace Native -Name U32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[StructLayout(LayoutKind.Sequential)] public struct CURSORINFO { public int cbSize; public int flags; public IntPtr hCursor; public int x; public int y; }
[StructLayout(LayoutKind.Sequential)] public struct ICONINFO { public bool fIcon; public int xHotspot; public int yHotspot; public IntPtr hbmMask; public IntPtr hbmColor; }
[StructLayout(LayoutKind.Sequential)] public struct BITMAP { public int bmType; public int bmWidth; public int bmHeight; public int bmWidthBytes; public short bmPlanes; public short bmBitsPixel; public IntPtr bmBits; }
[DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO ci);
[DllImport("user32.dll")] public static extern bool GetIconInfo(IntPtr h, out ICONINFO ii);
[DllImport("user32.dll")] public static extern bool DrawIconEx(IntPtr hdc, int x, int y, IntPtr h, int w, int hgt, int step, IntPtr brush, int flags);
[DllImport("gdi32.dll")] public static extern int GetObject(IntPtr h, int cb, out BITMAP bm);
'@

# Draws the cursor the system is currently showing onto a screen crop whose top-left is (ox, oy).
function Add-Cursor($graphics, $ox, $oy) {
    $ci = New-Object Native.U32+CURSORINFO; $ci.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($ci)
    if (-not [Native.U32]::GetCursorInfo([ref]$ci) -or $ci.hCursor -eq [IntPtr]::Zero) { return "no cursor" }
    $ii = New-Object Native.U32+ICONINFO; [void][Native.U32]::GetIconInfo($ci.hCursor, [ref]$ii)
    $bm = New-Object Native.U32+BITMAP; [void][Native.U32]::GetObject($ii.hbmMask, 24, [ref]$bm)
    $hdc = $graphics.GetHdc()
    [void][Native.U32]::DrawIconEx($hdc, $ci.x - $ox - $ii.xHotspot, $ci.y - $oy - $ii.yHotspot, $ci.hCursor, 0, 0, 0, [IntPtr]::Zero, 3)
    $graphics.ReleaseHdc($hdc)
    "cursor handle 0x{0:x} flags {1} mask {2}x{3} colour={4} hotspot {5},{6}" -f [int64]$ci.hCursor, $ci.flags, $bm.bmWidth, $bm.bmHeight, ($ii.hbmColor -ne [IntPtr]::Zero), $ii.xHotspot, $ii.yHotspot
}
[void][Native.U32]::SetProcessDPIAware()

New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$game = Resolve-Path "$PSScriptRoot\..\.."
$proc = Start-Process -FilePath "$game\Imperialism II.exe" -WorkingDirectory $game -PassThru
Start-Sleep -Seconds $Wait

# The view is the largest 4:3 rectangle centred on the primary screen.
$b = [Windows.Forms.Screen]::PrimaryScreen.Bounds
$scale = [Math]::Min($b.Width / 800.0, $b.Height / 600.0)
$ox = ($b.Width - 800 * $scale) / 2
$oy = ($b.Height - 600 * $scale) / 2
$sx = [int]($ox + $GameX * $scale); $sy = [int]($oy + $GameY * $scale)

# Approach in small steps so the game sees ordinary mouse movement.
for ($i = 40; $i -ge 0; $i--) { [void][Native.U32]::SetCursorPos($sx - $i * 6, $sy - $i * 4); Start-Sleep -Milliseconds 20 }

# One capture straight after the move, one after the game has had time to settle.
foreach ($stage in 'moving', 'settled') {
    if ($stage -eq 'settled') { Start-Sleep -Seconds 2 }
    $bmp = New-Object Drawing.Bitmap($Crop, $Crop)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen(($sx - $Crop / 2), ($sy - $Crop / 2), 0, 0, $bmp.Size)
    $info = Add-Cursor $g ($sx - $Crop / 2) ($sy - $Crop / 2)
    $path = $Out -replace '\.png$', "_$stage.png"
    $bmp.Save($path, [Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    "$stage : $info -> $path"
}
"cursor at screen $sx,$sy (game $GameX,$GameY)"

if (-not $proc.HasExited) {
    [void]$proc.CloseMainWindow()
    if (-not $proc.WaitForExit(3000)) { Stop-Process -Id $proc.Id -Force }
}
