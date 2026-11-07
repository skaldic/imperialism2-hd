# Drives the game in remote-testing mode ([Debug] Remote=1 in the ini: its window is invisible and
# click-through) by posting messages to its view: nothing touches the real mouse, keyboard or screen.
# Positions are in the frame's game coordinates (1056x600 in wide mode).
#   wait:<seconds>   click:<x>,<y>   move:<x>,<y>   key:{ESC}|{ENTER}|{LEFT}|...|text   shot:<name>
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Steps,
    [string]$Game = "",
    [string]$Shots = ""
)
if (-not $Game) { $Game = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path }
if (-not $Shots) { $Shots = Join-Path $PSScriptRoot "..\hd\shots" }
Add-Type -Namespace BG -Name M -MemberDefinition '[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);'
$info = Get-Content "$Game\imp2hd-remote.txt" -ErrorAction Stop
$view = [IntPtr][long]($info -split ' ')[0]
$base = 0x8000
function Post([int]$msg, [long]$w, [long]$l) { [void][BG.M]::PostMessage($view, $base + $msg, [IntPtr]$w, [IntPtr]$l) }
function XY([string]$a) { $x, $y = $a -split ','; return ([long][int]$y -shl 16) -bor ([int]$x -band 0xffff) }
$keys = @{ '{ESC}' = 0x1b; '{ENTER}' = 0x0d; '{LEFT}' = 0x25; '{UP}' = 0x26; '{RIGHT}' = 0x27; '{DOWN}' = 0x28; '{TAB}' = 0x09; '{BACKSPACE}' = 0x08 }
New-Item -ItemType Directory -Force $Shots | Out-Null
foreach ($step in $Steps) {
    $verb, $arg = $step -split ':', 2
    switch ($verb) {
        'wait'  { Start-Sleep -Milliseconds ([int]([double]$arg * 1000)) }
        'move'  { Post 0x101 0 (XY $arg); Start-Sleep -Milliseconds 60 }
        'click' { $l = XY $arg; Post 0x101 0 $l; Start-Sleep -Milliseconds 120; Post 0x102 0 $l; Start-Sleep -Milliseconds 80; Post 0x103 0 $l }
        'key'   {
            if ($keys.ContainsKey($arg)) { Post 0x104 $keys[$arg] 0 }
            else { foreach ($c in $arg.ToCharArray()) { $vk = if ($c -eq '.') { 0xbe } else { [int][char]([string]$c).ToUpper() }; Post 0x104 $vk ([int]$c); Start-Sleep -Milliseconds 40 } }
        }
        'shot'  {
            $f = "$Game\imp2hd-shot.png"; Remove-Item $f -ErrorAction SilentlyContinue
            Post 0x110 1 0
            $t = 0; while (-not (Test-Path $f) -and $t -lt 100) { Start-Sleep -Milliseconds 100; $t++ }
            if (Test-Path $f) { Move-Item -Force $f (Join-Path $Shots "$arg.png"); "saved $arg.png" } else { "no frame for $arg" }
        }
        default { "unknown step '$step'" }
    }
}
