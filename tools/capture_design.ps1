param(
    [string]$Design = "EEG\physioamp_flash_puzzle",
    [string]$Out    = "E:\EE317-GYY\BrainBay\tools\_tmp\shot.png",
    [int]   $Wait   = 8
)

# Starts BrainBay with a design, presses IDM_PLAY so the session runs, waits,
# then grabs a picture of the BrainBay window and saves it as PNG.
# Used to compare designs visually (Flash panel working / failing).

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

Add-Type -Namespace BBWin -Name Api -MemberDefinition @"
    [DllImport("user32.dll", CharSet=CharSet.Auto)]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll", CharSet=CharSet.Auto)]
    public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string win);
    [DllImport("user32.dll", CharSet=CharSet.Auto)]
    public static extern int GetClassName(IntPtr hWnd, System.Text.StringBuilder name, int max);
    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
    public struct RECT { public int Left, Top, Right, Bottom; }
"@

$bin = "E:\EE317-GYY\BrainBay\bin"
$exe = Join-Path $bin "BrainBay.exe"
$cfg = Join-Path $bin "brainbay.cfg"
$bak = Join-Path $bin "brainbay.cfg.shot_backup"
$log = Join-Path $bin "bbay.log"

Get-Process BrainBay -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

Copy-Item $cfg $bak -Force
try {
    $txt = [IO.File]::ReadAllText($cfg)
    $txt = $txt -replace '(?m)^startdesignpath=.*$', ("startdesignpath=" + $Design + "`r")
    $txt = $txt -replace '(?m)^startdesign=.*$',     "startdesign=1`r"
    [IO.File]::WriteAllText($cfg, $txt)

    if (Test-Path $log) { Remove-Item $log -Force }

    $p = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
    Start-Sleep -Seconds 6

    $proc = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    $h = $proc.MainWindowHandle

    if ($h -eq 0) {
        "main window handle not found"
    } else {
        [void][BBWin.Api]::ShowWindow($h, 3)          # SW_MAXIMIZE
        [void][BBWin.Api]::SetForegroundWindow($h)
        Start-Sleep -Milliseconds 600

        # IDM_PLAY = 32830 -> start session
        [void][BBWin.Api]::PostMessage($h, 0x0111, [IntPtr]32830, [IntPtr]::Zero)
        Start-Sleep -Seconds $Wait

        # hide the design canvas so the display panels (incl. the Flash movie) show
        $sb = New-Object System.Text.StringBuilder 256
        $child = [IntPtr]::Zero
        do {
            $child = [BBWin.Api]::FindWindowEx($h, $child, $null, $null)
            if ($child -ne [IntPtr]::Zero) {
                [void][BBWin.Api]::GetClassName($child, $sb, 256)
                if ($sb.ToString() -eq "#32770") {
                    [void][BBWin.Api]::PostMessage($child, 0x0111, [IntPtr]1197, [IntPtr]::Zero)
                    "sent IDC_DESIGN to status box $child"
                    break
                }
            }
        } while ($child -ne [IntPtr]::Zero)
        Start-Sleep -Seconds 3

        $r = New-Object BBWin.Api+RECT
        [void][BBWin.Api]::GetWindowRect($h, [ref]$r)
        $w = $r.Right - $r.Left
        $ht = $r.Bottom - $r.Top
        "window = $($r.Left),$($r.Top) ${w}x${ht}"

        $bmp = New-Object System.Drawing.Bitmap $w, $ht
        $g   = [System.Drawing.Graphics]::FromImage($bmp)
        $hdc = $g.GetHdc()
        [void][BBWin.Api]::PrintWindow($h, $hdc, 2)      # PW_RENDERFULLCONTENT
        $g.ReleaseHdc($hdc)
        $g.Dispose()
        $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        "saved $Out"
    }

    Get-Process BrainBay -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 600
}
finally {
    Copy-Item $bak $cfg -Force
    Remove-Item $bak -Force
}

"----- log -----"
if (Test-Path $log) { Get-Content $log -Tail 12 }
