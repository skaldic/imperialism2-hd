# Pulls one screen's view tree out of imp2hd.log (written with [Debug] DumpViews=1) as
# "x y w h leaf" lines in window coordinates, for hd\layouttool preview.
#   -Child picks the screen whose dump contains a view with that id, e.g. -Child wool
param([Parameter(Mandatory)][string]$Child, [string]$Log, [string]$Out)
if (-not $Log) { $Log = Join-Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) "..\imp2hd.log" }
$lines = Get-Content $Log
$pattern = "^(\s+)(\S+) '(.{4})' at -?\d+,-?\d+ \(window (-?\d+),(-?\d+)\) size (\d+)x(\d+)"
$blocks = @(); $current = $null
foreach ($l in $lines) {
    if ($l -match '^screen ') { $current = New-Object System.Collections.ArrayList; $blocks += , $current; continue }
    if ($null -ne $current -and $l -match $pattern) {
        [void]$current.Add([pscustomobject]@{ Depth = $matches[1].Length; Id = $matches[3]; X = [int]$matches[4]; Y = [int]$matches[5]; W = [int]$matches[6]; H = [int]$matches[7] })
    }
}
# The id must belong to one of the screen's own views, not to a view nested deeper.
$block = $blocks | Where-Object { $b = $_; $b.Count -gt 0 -and ($b | Where-Object { $_.Id -eq $Child -and $_.Depth -eq $b[0].Depth + 2 }) } | Select-Object -Last 1
if (-not $block) { throw "no screen with a view '$Child' in $Log" }
$result = for ($i = 1; $i -lt $block.Count; $i++) {
    $v = $block[$i]
    $leaf = if ($i + 1 -lt $block.Count -and $block[$i + 1].Depth -gt $v.Depth) { 0 } else { 1 }
    "$($v.X) $($v.Y) $($v.W) $($v.H) $leaf"
}
if ($Out) { $result | Set-Content -Encoding ascii $Out } else { $result }
