# ---------------------------------------------------------------------------
#  load_test.ps1 - start BrainBay with a design as start design, watch the log,
#  then kill it and restore the original brainbay.cfg
#
#  usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\load_test.ps1
#         [-Design "EEG\physioamp_flash_neuropuzzles"] [-Seconds 12]
# ---------------------------------------------------------------------------
param(
    [string]$Design  = "EEG\physioamp_flash_neuropuzzles",
    [int]   $Seconds = 12
)

$ErrorActionPreference = "Stop"
$bin  = "E:\EE317-GYY\BrainBay\bin"
$exe  = Join-Path $bin "BrainBay.exe"
$cfg  = Join-Path $bin "brainbay.cfg"
$bak  = Join-Path $bin "brainbay.cfg.loadtest_backup"
$log  = Join-Path $bin "bbay.log"

if (Get-Process BrainBay -ErrorAction SilentlyContinue) {
    Write-Host "BrainBay is already running - close it first, nothing was changed." -ForegroundColor Yellow
    exit 3
}

Copy-Item $cfg $bak -Force
Write-Host "backed up brainbay.cfg"

try {
    $txt = [IO.File]::ReadAllText($cfg)
    $txt = $txt -replace '(?m)^startdesignpath=.*$', ("startdesignpath=" + $Design + "`r")
    $txt = $txt -replace '(?m)^startdesign=.*$',     "startdesign=1`r"
    [IO.File]::WriteAllText($cfg, $txt)

    if (Test-Path $log) { Remove-Item $log -Force }

    Write-Host "starting BrainBay with design '$Design' for $Seconds s ..."
    $p = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
    Start-Sleep -Seconds $Seconds

    $alive = -not $p.HasExited
    Write-Host ("still running after {0}s: {1}" -f $Seconds, $alive)

    if ($alive) { Stop-Process -Id $p.Id -Force }
    Start-Sleep -Milliseconds 800
}
finally {
    Copy-Item $bak $cfg -Force
    Remove-Item $bak -Force
    Write-Host "brainbay.cfg restored"
}

Write-Host "`n----- bbay.log -----"
if (Test-Path $log) { Get-Content $log } else { Write-Host "(no log file)" }
