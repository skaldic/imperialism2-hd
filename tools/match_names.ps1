# Carries the symbol names from the January 1999 linker map over to the shipped (v1.03) exe.
# The two builds share link order, so the address shift between them is piecewise constant and
# mostly grows as patched functions got longer. A Viterbi pass over (map function, shift) picks
# the shift sequence that lands the most map functions on real function starts in the exe,
# charging a penalty every time the shift changes.
param(
    [string]$MapCsv = "$PSScriptRoot\..\symbols_map_jan1999.csv",
    [string]$ExeCsv = "$PSScriptRoot\..\functions_v103.csv",
    [string]$OutCsv = "$PSScriptRoot\..\names_v103.csv",
    [string]$ExePath = "$PSScriptRoot\..\..\Imperialism II.exe"
)

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;

public static class ShiftMatcher
{
    const int States = 5120;      // shifts 0 .. 0x14000 in 16-byte steps
    const int GrowPenalty = 3;    // hits a larger shift must earn back
    const int ShrinkPenalty = 4;
    const int MaxShrink = 32;     // a shift may drop by at most 0x200 in one step

    // Ghidra misses small functions that are only reached through vtables, so also take every
    // 16-aligned address in .text that follows inter-function padding (int3/nop).
    public static void AddPaddingStarts(byte[] exe, HashSet<uint> starts, uint textVa, int textRaw, int textLen)
    {
        for (int o = textRaw + 16; o < textRaw + textLen; o += 16)
        {
            byte c = exe[o], p = exe[o - 1];
            if (c == 0xCC || c == 0x90 || c == 0x00) continue;
            if (p == 0xCC || p == 0x90) starts.Add(textVa + (uint)(o - textRaw));
        }
    }

    // Returns the chosen shift (in bytes) for every map address.
    public static int[] Run(uint[] addrs, HashSet<uint> exe)
    {
        int n = addrs.Length;
        int[] prev = new int[States], cur = new int[States];
        ushort[] back = new ushort[(long)n * States];

        for (int s = 0; s < States; s++)
            prev[s] = exe.Contains(addrs[0] + (uint)(s * 16)) ? 1 : 0;

        for (int i = 1; i < n; i++)
        {
            int bestLower = int.MinValue, bestLowerAt = 0;
            long row = (long)i * States;
            for (int s = 0; s < States; s++)
            {
                int best = prev[s], from = s;
                if (bestLower != int.MinValue && bestLower - GrowPenalty > best)
                {
                    best = bestLower - GrowPenalty; from = bestLowerAt;
                }
                int hi = Math.Min(States - 1, s + MaxShrink);
                for (int t = s + 1; t <= hi; t++)
                {
                    if (prev[t] - ShrinkPenalty > best) { best = prev[t] - ShrinkPenalty; from = t; }
                }
                cur[s] = best + (exe.Contains(addrs[i] + (uint)(s * 16)) ? 1 : 0);
                back[row + s] = (ushort)from;
                if (prev[s] > bestLower) { bestLower = prev[s]; bestLowerAt = s; }
            }
            int[] tmp = prev; prev = cur; cur = tmp;
        }

        int end = 0;
        for (int s = 1; s < States; s++) if (prev[s] > prev[end]) end = s;

        int[] shifts = new int[n];
        for (int i = n - 1; i >= 0; i--)
        {
            shifts[i] = end * 16;
            if (i > 0) end = back[(long)i * States + end];
        }
        return shifts;
    }
}
'@

$exeStarts = New-Object 'System.Collections.Generic.HashSet[uint32]'
foreach ($f in Import-Csv -LiteralPath $ExeCsv) {
    if ($f.is_thunk -eq '0') { [void]$exeStarts.Add([Convert]::ToUInt32($f.addr, 16)) }
}
$ghidraStarts = $exeStarts.Count
$exeBytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $ExePath))
[ShiftMatcher]::AddPaddingStarts($exeBytes, $exeStarts, 0x401000, 0x400, 0x2E784F)
"exe function starts    : $($exeStarts.Count) ($ghidraStarts from Ghidra)"

# One entry per distinct code address in the map, keeping every name that shares it.
$byAddr = @{}
foreach ($s in Import-Csv -LiteralPath $MapCsv) {
    if ($s.seg -ne '1' -or $s.is_func -ne '1') { continue }
    $a = [Convert]::ToUInt32($s.addr, 16)
    if (-not $byAddr.ContainsKey($a)) { $byAddr[$a] = New-Object System.Collections.ArrayList }
    [void]$byAddr[$a].Add($s)
}
$addrs = [uint32[]]@($byAddr.Keys)
[Array]::Sort($addrs)

$shifts = [ShiftMatcher]::Run($addrs, $exeStarts)

# A miss inside a run of constant shift is still placed: the function is there, it just has no
# padding in front of it. "hit" records whether the exe independently shows a start at that spot.
$hits = 0; $changes = 0
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('addr,map_addr,shift,hit,obj,name')
for ($i = 0; $i -lt $addrs.Count; $i++) {
    $t = [uint32]($addrs[$i] + $shifts[$i])
    $hit = [int]$exeStarts.Contains($t)
    $hits += $hit
    if ($i -gt 0 -and $shifts[$i] -ne $shifts[$i - 1]) { $changes++ }
    foreach ($s in $byAddr[$addrs[$i]]) {
        [void]$sb.AppendLine(('{0:x8},{1:x8},{2:x},{3},{4},{5}' -f $t, $addrs[$i], $shifts[$i], $hit, $s.obj, $s.name))
    }
}

[IO.File]::WriteAllText($OutCsv, $sb.ToString())
"map function addresses : $($addrs.Count)"
"landed on a real start : $hits"
"shift changes          : $changes"
"final shift            : 0x{0:X}" -f $shifts[$addrs.Count - 1]
