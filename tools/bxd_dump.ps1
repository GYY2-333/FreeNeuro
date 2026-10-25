param(
    [Parameter(Mandatory=$true)][string]$Folder,
    [Parameter(Mandatory=$true)][string]$OutFile,
    [string]$Filter = "",
    [int]$MinLen = 3
)

$ErrorActionPreference = "Stop"
$out = New-Object System.Collections.ArrayList
function Add-Line([string]$s) { [void]$out.Add($s) }

foreach ($f in (Get-ChildItem -Path $Folder -Recurse -Filter "*.bxd")) {
    $rel = $f.FullName.Substring($Folder.Length).TrimStart('\')
    Add-Line ("===== " + $rel + "  (" + $f.Length + " bytes)")

    $bytes = [IO.File]::ReadAllBytes($f.FullName)
    $uni = [Text.Encoding]::Unicode.GetString($bytes)
    $asc = [Text.Encoding]::GetEncoding(1252).GetString($bytes)

    $set = New-Object System.Collections.Hashtable
    foreach ($m in [regex]::Matches($uni, "[\x20-\x7E\u4e00-\u9fff]{$MinLen,}")) {
        $v = $m.Value.Trim()
        if ($v.Length -ge $MinLen) { $set[$v] = 1 }
    }
    foreach ($m in [regex]::Matches($asc, "[\x20-\x7E]{4,}")) {
        $v = $m.Value.Trim()
        if ($v.Length -ge 4) { $set[$v] = 1 }
    }

    $keys = @($set.Keys)
    if ($Filter -ne "") { $keys = @($keys | Where-Object { $_ -match $Filter }) }
    $keys = @($keys | Sort-Object)
    foreach ($k in $keys) { Add-Line ("  " + $k) }
    Add-Line ""
}

$out | Set-Content -Path $OutFile -Encoding ASCII
Write-Output ("written " + $OutFile + " (" + $out.Count + " lines)")
