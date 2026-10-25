# ---------------------------------------------------------------------------
#  swf_strings.ps1 - list printable strings inside a .swf (FWS or CWS)
#
#  CWS files are zlib-compressed behind an 8 byte header; this script inflates
#  the body in memory and extracts ASCII/UTF-8 runs, so that the ActionScript
#  constant pool (variable names, frame labels, button texts) becomes readable
#  without a full decompiler.
#
#  usage:
#    powershell -File tools\swf_strings.ps1 -Path <file.swf> [-Filter "play|start"]
# ---------------------------------------------------------------------------
param(
    [Parameter(Mandatory=$true)][string]$Path,
    [string]$Filter = "",
    [int]$MinLen = 4,
    [switch]$Dump
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression | Out-Null

if (-not (Test-Path $Path)) { Write-Host "not found: $Path" -ForegroundColor Red; exit 2 }

$b = [IO.File]::ReadAllBytes($Path)
$magic = [string]([char]$b[0]) + [string]([char]$b[1]) + [string]([char]$b[2])
Write-Host "file  : $Path"
Write-Host "size  : $($b.Length) bytes, magic $magic"

if ($magic -eq 'CWS') {
    $body = New-Object byte[] ($b.Length - 8)
    [Array]::Copy($b, 8, $body, 0, $b.Length - 8)
    # CWS stores a zlib stream (0x78 ..) - DeflateStream wants raw deflate, so
    # the 2 byte zlib header and the trailing adler32 are cut off
    $start = 0
    if ($body[0] -eq 0x78) { $start = 2 }
    $len = $body.Length - $start - 4
    $raw = New-Object byte[] $len
    [Array]::Copy($body, $start, $raw, 0, $len)
    $in  = New-Object IO.MemoryStream(,$raw)
    $ds  = New-Object IO.Compression.DeflateStream($in, [IO.Compression.CompressionMode]::Decompress)
    $out = New-Object IO.MemoryStream
    try {
        $ds.CopyTo($out)
    } catch {
        Write-Host "inflate failed: $($_.Exception.Message)" -ForegroundColor Yellow
    }
    $ds.Dispose()
    $data = $out.ToArray()
    Write-Host "inflated: $($data.Length) bytes"
} elseif ($magic -eq 'FWS') {
    $data = $b
} elseif ($magic -eq 'ZWS') {
    Write-Host "LZMA compressed (ZWS) - not supported here" -ForegroundColor Yellow
    exit 4
} else {
    Write-Host "not a swf" -ForegroundColor Red
    exit 3
}

$txt = [Text.Encoding]::GetEncoding(1252).GetString($data)
$m = [regex]::Matches($txt, "[\x20-\x7E]{$MinLen,}")
$all = @()
foreach ($x in $m) { $all += $x.Value }
$uniq = $all | Select-Object -Unique
Write-Host "strings: $($all.Count) total, $($uniq.Count) unique`n"

if ($Dump) { $uniq }
elseif ($Filter -ne "") {
    $uniq | Where-Object { $_ -match $Filter }
}
else {
    $uniq | Select-Object -First 200
}
