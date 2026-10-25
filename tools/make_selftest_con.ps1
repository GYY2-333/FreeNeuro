param(
    [string]$Source = "E:\EE317-GYY\BrainBay\bin\CONFIGURATIONS\EEG\physioamp_flash_neuropuzzles.con",
    [string]$Target = "E:\EE317-GYY\BrainBay\bin\CONFIGURATIONS\EEG\physioamp_selftest.con"
)

$ErrorActionPreference = "Stop"
$txt = [IO.File]::ReadAllText($Source)

# verification copy: internal test signal on, session starts automatically
$txt = $txt -replace '(?m)^bp_selftest=.*$', 'bp_selftest=1'
$txt = $txt -replace '(?m)^autorun=.*$',    'autorun=1'
$txt = $txt -replace '(?m)^bp_logvalues=.*$','bp_logvalues=1'

[IO.File]::WriteAllText($Target, $txt)
Write-Output ("written " + $Target)
Select-String -Path $Target -Pattern '^bp_selftest=|^autorun=|^bp_logvalues=' | ForEach-Object { Write-Output ("  " + $_.Line) }
