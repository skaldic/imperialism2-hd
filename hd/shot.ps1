# Launches the game, takes screenshots at the given delays (seconds after launch), then closes it.
# Screenshots are saved half-size as PNG in the output folder.
param(
    [int[]]$At = @(8, 20),
    [string]$OutDir = "$PSScriptRoot\shots",
    [switch]$KeepRunning,
    [int[]]$EscAt = @()   # seconds after launch at which to press Esc (skips the intro movies)
)

Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type -Namespace Native -Name Dpi -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();'
[void][Native.Dpi]::SetProcessDPIAware()

New-Item -ItemType Directory -Force $OutDir | Out-Null
$game = Resolve-Path "$PSScriptRoot\..\.."
$proc = Start-Process -FilePath "$game\Imperialism II.exe" -WorkingDirectory $game -PassThru

$bounds = [Windows.Forms.Screen]::PrimaryScreen.Bounds
$elapsed = 0
foreach ($t in (@($At) + @($EscAt) | Sort-Object -Unique)) {
    Start-Sleep -Seconds ($t - $elapsed)
    $elapsed = $t
    if ($proc.HasExited) { "game exited early (code $($proc.ExitCode))"; break }
    if ($EscAt -contains $t) { [Windows.Forms.SendKeys]::SendWait("{ESC}") }
    if ($At -notcontains $t) { continue }
    $full = New-Object Drawing.Bitmap($bounds.Width, $bounds.Height)
    $g = [Drawing.Graphics]::FromImage($full)
    $g.CopyFromScreen($bounds.Location, [Drawing.Point]::Empty, $bounds.Size)
    $half = New-Object Drawing.Bitmap($full, [int]($bounds.Width / 2), [int]($bounds.Height / 2))
    $path = Join-Path $OutDir "shot_${t}s.png"
    $half.Save($path, [Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $full.Dispose(); $half.Dispose()
    "saved $path (screen $($bounds.Width)x$($bounds.Height))"
}

if (-not $KeepRunning -and -not $proc.HasExited) {
    [void]$proc.CloseMainWindow()
    if (-not $proc.WaitForExit(3000)) { Stop-Process -Id $proc.Id -Force }
    "game closed"
}
