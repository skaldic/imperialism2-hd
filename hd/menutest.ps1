# Wide-mode check of a centred screen: opens the load screen from the main menu with paint
# tracing on, screenshots it, and prints what the DLL logged from the moment the screen appeared.
# Whatever imp2hd.ini was in the game folder is put back afterwards.
param([string]$Shot = "menu_load")

$game = Resolve-Path "$PSScriptRoot\..\.."
$ini = "$game\imp2hd.ini"
$saved = if (Test-Path -LiteralPath $ini) { [IO.File]::ReadAllText($ini) } else { $null }
Set-Content -LiteralPath $ini -Value "[Display]`r`nWide=1`r`n[Debug]`r`nDumpViews=1" -Encoding ascii
try {
    & "$PSScriptRoot\drive.ps1" -GameW 1056 'wait:6' 'click:133,5' 'wait:2' 'click:133,5' 'wait:4' `
        'click:278,450' 'wait:4' "shot:$Shot" 'move:600,300' 'wait:2' "shot:${Shot}_moved"
}
finally {
    if ($null -ne $saved) { [IO.File]::WriteAllText($ini, $saved) } else { Remove-Item -LiteralPath $ini -Force }
}
$log = Get-Content "$game\imp2hd.log"
$start = ($log | Select-String 'TGameSetupPicture' | Select-Object -First 1).LineNumber
if ($start) {
    $log[([Math]::Max(0, $start - 4))..($log.Count - 1)] | Where-Object { $_ -notmatch '^ {4}' } | Select-Object -First 40
} else {
    "the load screen was never created"
    $log | Where-Object { $_ -match '^screen |^press|^release' }
}
