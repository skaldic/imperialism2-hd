# Loads the autosave in wide mode with the given re-layout steps enabled, screenshots the map,
# and prints the result. Extra drive.ps1 steps (clicks, keys, shots) can follow once the map is up.
# Whatever imp2hd.ini was in the game folder is put back afterwards.
param(
    [int]$Steps = 7,
    [string]$Shot = "wide_steps$Steps",
    [string[]]$Then = @()
)

$game = Resolve-Path "$PSScriptRoot\..\.."
$ini = "$game\imp2hd.ini"
$saved = if (Test-Path -LiteralPath $ini) { [IO.File]::ReadAllText($ini) } else { $null }
Set-Content -LiteralPath $ini -Value "[Display]`r`nWide=1`r`n[Debug]`r`nDumpViews=1`r`nWideSteps=$Steps" -Encoding ascii
try {
    $script = @('wait:6', 'click:5,5', 'wait:2', 'click:5,5', 'wait:4', 'click:150,450', 'wait:4',
        'click:643,475', 'wait:2', 'click:400,72', 'wait:9', "shot:$Shot") + $Then + @('wait:1')
    & "$PSScriptRoot\drive.ps1" -GameW 1056 @script
}
finally {
    if ($null -ne $saved) { [IO.File]::WriteAllText($ini, $saved) } else { Remove-Item -LiteralPath $ini -Force }
}
$log = Get-Content "$game\imp2hd.log"
$log | Where-Object { $_ -match 'widened|wide mode|parking' }
$log | Where-Object { $_ -match "^ {8}\S" -and $_ -match "'" } | ForEach-Object { $_ -replace '\s+\[[0-9a-f]+\]$', '' }
