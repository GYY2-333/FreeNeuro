param(
    [string]$Design = "EEG\physioamp_flash_puzzle",
    [string]$Out    = "E:\EE317-GYY\BrainBay\tools\_tmp\tiled2.png",
    [int]   $Wait   = 6
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

Add-Type -Namespace BBC -Name Api -MemberDefinition @"
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Auto)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Auto)] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    public struct RECT { public int Left, Top, Right, Bottom; }
"@

$report = "E:\EE317-GYY\BrainBay\tools\_tmp\shot2.log"
if (Test-Path $report) { Remove-Item $report -Force }
function Say($t) { [IO.File]::AppendAllText($report, "$t`r`n") }

$bin = Join-Path "E:\EE317-GYY\BrainBay" "bin"
$exe = Join-Path $bin "BrainBay.exe"
$cfg = Join-Path $bin "brainbay.cfg"
$bak = Join-Path $bin "brainbay.cfg.shot_backup"

try {
    Get-Process BrainBay -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    Copy-Item $cfg $bak -Force

    $txt = [IO.File]::ReadAllText($cfg)
    $txt = $txt -replace '(?m)^startdesignpath=.*$', ("startdesignpath=" + $Design + "`r")
    $txt = $txt -replace '(?m)^startdesign=.*$',     "startdesign=1`r"
    [IO.File]::WriteAllText($cfg, $txt)

    $p = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
    Start-Sleep -Seconds 8
    $main = [IntPtr](Get-Process -Id $p.Id).MainWindowHandle
    Say "main=$main pid=$($p.Id)"

    $target = [int]$p.Id
    $global:found = @()
    $cb = {
        param([IntPtr]$h, [IntPtr]$p2)
        $pid2 = 0
        [void][BBC.Api]::GetWindowThreadProcessId($h, [ref]$pid2)
        if ($pid2 -ne $target) { return $true }
        $cls = New-Object System.Text.StringBuilder 256
        [void][BBC.Api]::GetClassName($h, $cls, 256)
        $global:found += ("{0}:{1}" -f $cls.ToString(), $h.ToString())
        return $true
    }
    [void][BBC.Api]::EnumChildWindows($main, $cb, [IntPtr]::Zero)
    Say ("children: " + ($global:found -join " | "))

    $dg = [IntPtr]::Zero
    foreach ($f in $global:found) {
        if ($f.StartsWith("Design_Class:")) { $dg = [IntPtr][int64]($f.Split(':')[1]) }
    }
    Say "design=$dg"
    if ($dg -ne [IntPtr]::Zero) { [void][BBC.Api]::ShowWindow($dg, 0); Say "design hidden" }

    [void][BBC.Api]::SetForegroundWindow($main)
    Start-Sleep -Seconds 1
    [void][BBC.Api]::PostMessage($main, 0x0111, [IntPtr]32830, [IntPtr]::Zero)
    Say "play posted"
    Start-Sleep -Seconds $Wait

    $r = New-Object BBC.Api+RECT
    [void][BBC.Api]::GetWindowRect($main, [ref]$r)
    $w = $r.Right - $r.Left
    $hgt = $r.Bottom - $r.Top
    Say "rect=$($r.Left),$($r.Top),$w,$hgt"

    $bmp = New-Object System.Drawing.Bitmap $w, $hgt
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size($w, $hgt)))
    $g.Dispose()
    $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Say "saved $Out"

    Get-Process BrainBay -ErrorAction SilentlyContinue | Stop-Process -Force
}
catch {
    Say ("ERROR: " + $_.Exception.Message)
}
finally {
    if (Test-Path $bak) { Copy-Item $bak $cfg -Force; Remove-Item $bak -Force }
}
Say "done"
