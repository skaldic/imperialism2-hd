# Converts the linker map that ships with the game ("Imperialism II.map", January 1999 build) into
# a CSV of public symbols. Code symbols get a size: the distance to the next code address.
param(
    [string]$MapPath = "$PSScriptRoot\..\..\Imperialism II.map",
    [string]$OutCsv = "$PSScriptRoot\..\symbols_map_jan1999.csv"
)

$rx = [regex]'^\s(\d{4}):([0-9a-f]{8})\s+(\S+)\s+([0-9a-f]{8})\s(.{3})\s(\S+)\s*$'
$rows = foreach ($line in [IO.File]::ReadLines((Resolve-Path -LiteralPath $MapPath))) {
    $m = $rx.Match($line)
    if ($m.Success) {
        [pscustomobject]@{
            Seg    = [int]$m.Groups[1].Value
            Addr   = [Convert]::ToUInt32($m.Groups[4].Value, 16)
            Name   = $m.Groups[3].Value
            IsFunc = ($m.Groups[5].Value -match 'f')
            Obj    = $m.Groups[6].Value
        }
    }
}

$starts = $rows | Where-Object { $_.Seg -eq 1 } | Sort-Object Addr | Select-Object -ExpandProperty Addr -Unique
$next = @{}
for ($i = 0; $i -lt $starts.Count - 1; $i++) { $next[$starts[$i]] = $starts[$i + 1] }

$sb = New-Object Text.StringBuilder
[void]$sb.AppendLine('seg,addr,size,is_func,obj,name')
foreach ($r in ($rows | Sort-Object Seg, Addr)) {
    $size = ''
    if ($r.Seg -eq 1 -and $next.ContainsKey($r.Addr)) { $size = $next[$r.Addr] - $r.Addr }
    [void]$sb.AppendLine(('{0},{1:x8},{2},{3},{4},{5}' -f $r.Seg, $r.Addr, $size, [int]$r.IsFunc, $r.Obj, $r.Name))
}
[IO.File]::WriteAllText($OutCsv, $sb.ToString())
"$($rows.Count) symbols -> $OutCsv"
