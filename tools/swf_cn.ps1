param(
    [Parameter(Mandatory=$true)][string]$Path,
    [string]$Filter = "修复|组件|Flash|flash|错误|失败|正常",
    [int]$MinLen = 2
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression | Out-Null

if (-not (Test-Path $Path)) { Write-Host "not found: $Path"; exit 2 }

$b = [IO.File]::ReadAllBytes($Path)
$magic = [string]([char]$b[0]) + [string]([char]$b[1]) + [string]([char]$b[2])
Write-Output "file: $Path  magic=$magic  size=$($b.Length)"

if ($magic -eq 'CWS') {
    $body = New-Object byte[] ($b.Length - 8)
    [Array]::Copy($b, 8, $body, 0, $b.Length - 8)
    $start = 0
    if ($body[0] -eq 0x78) { $start = 2 }
    $len = $body.Length - $start - 4
    $raw = New-Object byte[] $len
    [Array]::Copy($body, $start, $raw, 0, $len)
    $in  = New-Object IO.MemoryStream(,$raw)
    $ds  = New-Object IO.Compression.DeflateStream($in, [IO.Compression.CompressionMode]::Decompress)
    $out = New-Object IO.MemoryStream
    try { $ds.CopyTo($out) } catch { Write-Output ("inflate warning: " + $_.Exception.Message) }
    $ds.Dispose()
    $data = $out.ToArray()
} else {
    $data = $b
}

Write-Output "inflated bytes: $($data.Length)"

# UTF-8 view catches the Chinese literals, 1252 view catches plain ASCII
$u8  = [Text.Encoding]::UTF8.GetString($data)
$asc = [Text.Encoding]::GetEncoding(1252).GetString($data)

$set = New-Object System.Collections.Hashtable
foreach ($m in [regex]::Matches($u8, "[\u4e00-\u9fff][\u4e00-\u9fff\x20-\x7E]{$MinLen,}")) { $set[$m.Value] = 1 }
foreach ($m in [regex]::Matches($u8, "[\x20-\x7E]{6,}")) { $set[$m.Value] = 1 }

$keys = @($set.Keys)
if ($Filter -ne "") { $keys = @($keys | Where-Object { $_ -match $Filter }) }
$keys = @($keys | Sort-Object)

Write-Output "matches: $($keys.Count)"
foreach ($k in $keys) { Write-Output ("  " + $k) }
