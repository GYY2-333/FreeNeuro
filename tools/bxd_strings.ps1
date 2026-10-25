# ---------------------------------------------------------------------------
#  bxd_strings.ps1 - dump readable strings from BioExplorer .bxd designs
#
#  .bxd files are OLE2 compound documents; the property records store their
#  names/values as UTF-16LE, so the file is decoded as Unicode and the readable
#  runs are extracted. Useful to see which .swf / variables / thresholds a
#  built-in model uses, without opening BioExplorer.
#
#  usage: powershell -File tools\bxd_strings.ps1 -Folder "<dir>" [-Filter "swf|in1"]
# ---------------------------------------------------------------------------
param(
    [Parameter(Mandatory=$true)][string]$Folder,
    [string]$Filter = "",
    [int]$MinLen = 3
)

$ErrorActionPreference = "Stop"

Get-ChildItem -Path $Folder -Recurse -Filter "*.bxd" | ForEach-Object {
    $f = $_
    Write-Output ("===== " + $f.FullName.Replace($Folder, "").TrimStart("\") + "  (" + $f.Length + " bytes)")

    $bytes = [IO.File]::ReadAllBytes($f.FullName)

    # UTF-16LE view (property names, string values)
    $uni = [Text.Encoding]::Unicode.GetString($bytes)
    $u = [regex]::Matches($uni, "[\x20-\x7E\u4e00-\u9fff]{$MinLen,}") |
         ForEach-Object { $_.Value.Trim() } | Where-Object { $_.Length -ge $MinLen } | Select-Object -Unique

    # ASCII view (some counters/paths are written as plain 8 bit)
    $asc = [Text.Encoding]::GetEncoding(1252).GetString($bytes)
    $a = [regex]::Matches($asc, "[\x20-\x7E]{4,}") | ForEach-Object { $_.Value } | Select-Object -Unique

    $all = @($u) + @($a)

    if ($Filter -ne "") {
        $all = $all | Where-Object { $_ -match $Filter }
    }

    $all | Select-Object -Unique | ForEach-Object { Write-Output ("  " + $_) }
    Write-Output ""
}
