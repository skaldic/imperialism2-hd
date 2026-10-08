# Launches the game and plays back a list of steps, for testing without a person at the mouse.
#   wait:<seconds>      pause
#   click:<x>,<y>       left-click at a point given in 800x600 game coordinates
#   move:<x>,<y>        move the mouse there without clicking
#   key:<sendkeys>      press a key, e.g. key:{ESC}
#   shot:<name>         save a half-size screenshot as shots\<name>.png
# Example: .\drive.ps1 wait:9 click:150,450 wait:3 shot:load
param(
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Steps,
    [switch]$KeepRunning,
    [int]$GameW = 800   # width of the game's frame: 1056 when testing wide mode
)

Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type -Namespace Native -Name Drive -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
'@
[void][Native.Drive]::SetProcessDPIAware()

$outDir = "$PSScriptRoot\shots"
New-Item -ItemType Directory -Force $outDir | Out-Null
$game = Resolve-Path "$PSScriptRoot\..\.."
$proc = Start-Process -FilePath "$game\Imperialism II.exe" -WorkingDirectory $game -PassThru

# The view is the largest rectangle of the game's shape centred on the primary screen.
$b = [Windows.Forms.Screen]::PrimaryScreen.Bounds
$scale = [Math]::Min($b.Width / [double]$GameW, $b.Height / 600.0)
$ox = ($b.Width - $GameW * $scale) / 2
$oy = ($b.Height - 600 * $scale) / 2

function Move-To([int]$gx, [int]$gy) {
    $sx = [int]($ox + $gx * $scale); $sy = [int]($oy + $gy * $scale)
    # Arrive in a few steps so the game sees ordinary mouse movement.
    for ($i = 8; $i -ge 0; $i--) { [void][Native.Drive]::SetCursorPos($sx - $i * 5, $sy - $i * 3); Start-Sleep -Milliseconds 25 }
}

foreach ($step in $Steps) {
    if ($proc.HasExited) { "game exited (code $($proc.ExitCode)) before '$step'"; break }
    $verb, $arg = $step -split ':', 2
    switch ($verb) {
        'wait'  { Start-Sleep -Milliseconds ([int]([double]$arg * 1000)) }
        'move'  { $x, $y = $arg -split ','; Move-To $x $y }
        'click' {
            $x, $y = $arg -split ','; Move-To $x $y
            Start-Sleep -Milliseconds 150
            [Native.Drive]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
            Start-Sleep -Milliseconds 80
            [Native.Drive]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
        }
        'key'   { [Windows.Forms.SendKeys]::SendWait($arg) }
        'shot'  {
            $full = New-Object Drawing.Bitmap($b.Width, $b.Height)
            $g = [Drawing.Graphics]::FromImage($full)
            $g.CopyFromScreen($b.Location, [Drawing.Point]::Empty, $b.Size)
            $half = New-Object Drawing.Bitmap($full, [int]($b.Width / 2), [int]($b.Height / 2))
            $half.Save("$outDir\$arg.png", [Drawing.Imaging.ImageFormat]::Png)
            $g.Dispose(); $full.Dispose(); $half.Dispose()
            "saved $arg.png"
        }
        default { "unknown step '$step'" }
    }
}

if (-not $KeepRunning -and -not $proc.HasExited) {
    [void]$proc.CloseMainWindow()
    if (-not $proc.WaitForExit(3000)) { Stop-Process -Id $proc.Id -Force }
}
