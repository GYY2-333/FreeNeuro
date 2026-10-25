# ---------------------------------------------------------------------------
#  validate_con.ps1 - structural check of a BrainBay .con design file
#
#  Repeats what load_next_config_buffer()/load_property() do and checks the
#  things that would silently break a design: object type sequence, property
#  presence, link targets, port counts.
#
#  usage:  powershell -ExecutionPolicy Bypass -File tools\validate_con.ps1 <file.con>
# ---------------------------------------------------------------------------
param(
    [Parameter(Mandatory=$true)][string]$Path,
    [string]$ExpectedTypes = ""
)

$ErrorActionPreference = "Stop"
$fail = 0
function Bad($msg) { Write-Host "  FAIL  $msg" -ForegroundColor Red; $script:fail++ }
function Ok ($msg) { Write-Host "  ok    $msg" -ForegroundColor Green }

if (-not (Test-Path $Path)) { Write-Host "file not found: $Path" -ForegroundColor Red; exit 2 }

$txt = [IO.File]::ReadAllText($Path)
Write-Host "file : $Path"
Write-Host "size : $($txt.Length) bytes"

$blocks = @($txt -split 'end object')
# the last chunk after the final "end object" is normally empty
while ($blocks.Count -gt 0 -and $blocks[$blocks.Count-1].Trim().Length -eq 0) {
    $blocks = $blocks[0..($blocks.Count-2)]
}
$header = $blocks[0]
$objects = @($blocks[1..($blocks.Count-1)])
Write-Host "blocks: header + $($objects.Count) object(s)"

function Get-Props($block) {
    $h = New-Object 'System.Collections.Hashtable'
    $h['__links'] = New-Object 'System.Collections.ArrayList'
    foreach ($line in ($block -split "`n")) {
        $l = $line.Trim()
        if ($l.Length -eq 0) { continue }
        if ($l -match '^(?i)linkport\s') { [void]$h['__links'].Add($l); continue }
        if ($l -match '^(?i)next\s+object\s*=') { continue }
        $i = $l.IndexOf('=')
        if ($i -gt 0) {
            $k = $l.Substring(0, $i).Trim().ToLower()
            $h[$k] = $l.Substring($i + 1).Trim()
        }
    }
    return $h
}

# ---- header ---------------------------------------------------------------
Write-Host "`n[header]"
$hp = Get-Props $header
if ($hp.ContainsKey('objects')) {
    if ([int]$hp['objects'] -eq $objects.Count) { Ok "objects=$($hp['objects']) matches block count" }
    else { Bad "objects=$($hp['objects']) but $($objects.Count) object blocks found" }
} else { Bad "objects= is missing" }

foreach ($k in @('devicetype','samplingrate','design-right','main-right')) {
    if (-not $hp.ContainsKey($k)) { Bad "$k is missing" }
}
if ($hp.ContainsKey('devicetype')) {
    if ([int]$hp['devicetype'] -eq 15) { Ok "devicetype=15 (PhysioAmp GP-8 HID)" }
    else { Bad "devicetype=$($hp['devicetype']), expected 15 for PhysioAmp" }
}
if ($hp.ContainsKey('samplingrate')) {
    if ([int]$hp['samplingrate'] -eq 3000) { Ok "samplingrate=3000" }
    else { Bad "samplingrate=$($hp['samplingrate']), expected 3000" }
}

# ---- objects --------------------------------------------------------------
$types = @()
for ($i = 0; $i -lt $objects.Count; $i++) {
    $m = [regex]::Match($objects[$i], '(?i)next\s+object\s*=\s*(-?\d+)')
    if (-not $m.Success) { Bad "object #$i has no 'next object=' type" ; $types += -1 }
    else { $types += [int]$m.Groups[1].Value }
}
Write-Host "`ntypes: $($types -join ', ')"

if ($ExpectedTypes -ne "") {
    $exp = $ExpectedTypes -split ',' | ForEach-Object { [int]$_.Trim() }
    if (($exp -join ',') -eq ($types -join ',')) { Ok "type sequence matches the expectation" }
    else { Bad "type sequence is $($types -join ','), expected $($exp -join ',')" }
}

# required properties per object type
$required = @{
    '0'  = @('outport1min','outport1max','outport1range','resolution')
    '76' = @('inport1min','inport1max','outport1min','outport1max',
             'ar_ppthreshold','ar_blockms','ar_emgenable','ar_emgthreshold',
             'ar_emgcutoff','ar_cliplevel')
    '75' = @('inport1min','inport1max','inport2min','inport2max',
             'outport3min','outport3max','outport12min','outport12max',
             'bp_fftsecs','bp_window','bp_hopms','bp_units','bp_notchfreq',
             'bp_betalo','bp_betahi')
    '3'  = @('inport1min','inport1max','from-input','to-input','and/or',
             'truemode','falsemode','numerictruevalue','numericfalsevalue',
             'show-meter','top','left','right','bottom','wndcaption')
    '78' = @('inport1min','inport1max','inport2min','inport2max',
             'swfpath','channels','autoplay','var1','var2',
             'left','top','right','bottom')
}

$names = @{ '0'='EEG'; '3'='THRESHOLD'; '10'='AND'; '75'='BAND-POWER'; '76'='ARTIFACT'; '78'='FLASH' }

Write-Host ""
for ($i = 0; $i -lt $objects.Count; $i++) {
    $t = "$($types[$i])"
    $p = Get-Props $objects[$i]
    $nm = if ($names.ContainsKey($t)) { $names[$t] } else { "type $t" }
    Write-Host "object #$i  $nm"

    if ($required.ContainsKey($t)) {
        foreach ($k in $required[$t]) {
            if (-not $p.ContainsKey($k)) { Bad "#$i ($nm) missing property '$k'" }
        }
    }

    # port counts vs. numbers actually described
    if ($p.ContainsKey('inputports')) {
        $n = [int]$p['inputports']
        for ($j = 1; $j -le $n; $j++) {
            if (-not $p.ContainsKey("inport${j}min")) { Bad "#$i ($nm) inputports=$n but inport${j}min missing" }
        }
    }
    if ($p.ContainsKey('outputports')) {
        $n = [int]$p['outputports']
        for ($j = 1; $j -le $n; $j++) {
            if (-not $p.ContainsKey("outport${j}min")) { Bad "#$i ($nm) outputports=$n but outport${j}min missing" }
        }
    }

    # links: "linkport from-to_object,to_port"
    foreach ($lk in $p['__links']) {
        $m = [regex]::Match($lk, '(?i)linkport\s+(-?\d+)\s*-\s*(-?\d+)\s*,\s*(-?\d+)')
        if (-not $m.Success) { Bad "#$i ($nm) malformed link '$lk'"; continue }
        $fromPort = [int]$m.Groups[1].Value
        $toObj    = [int]$m.Groups[2].Value
        $toPort   = [int]$m.Groups[3].Value

        if ($toObj -lt 0 -or $toObj -ge $objects.Count) {
            Bad "#$i ($nm) link '$lk' points to object $toObj which does not exist"
        } else {
            $tp = Get-Props $objects[$toObj]
            if ($tp.ContainsKey('inputports')) {
                if ($toPort -lt 0 -or $toPort -ge [int]$tp['inputports']) {
                    Bad "#$i ($nm) link '$lk' -> object $toObj has only $($tp['inputports']) input port(s)"
                }
            }
        }
        if ($p.ContainsKey('outputports')) {
            if ($fromPort -lt 0 -or $fromPort -ge [int]$p['outputports']) {
                Bad "#$i ($nm) link '$lk' uses output port $fromPort, object has $($p['outputports'])"
            }
        }
    }
    if ($p['__links'].Count -gt 0) { Ok "$($p['__links'].Count) link(s): $($p['__links'] -join ' | ')" }
}

# ---- specific design checks ----------------------------------------------
Write-Host "`n[design checks]"
if ($objects.Count -ge 7) {
    $flash = Get-Props $objects[$objects.Count - 1]
    if ($flash.ContainsKey('channels') -and $flash.ContainsKey('inputports')) {
        if ([int]$flash['channels'] -eq [int]$flash['inputports']) { Ok "FLASH channels == inputports ($($flash['channels']))" }
        else { Bad "FLASH channels=$($flash['channels']) but inputports=$($flash['inputports'])" }
    }
    if ($flash.ContainsKey('drive') -and [int]$flash['drive'] -ne 0) {
        Ok "FLASH drive mode on: In1 = score (every $($flash['scorems']) ms while gate open), In2 = clock (every $($flash['clockms']) ms)"
        if (-not $flash.ContainsKey('gateport')) { Bad "FLASH drive=1 but gateport missing" }
        elseif ([int]$flash['gateport'] -ge [int]$flash['inputports']) { Bad "FLASH gateport=$($flash['gateport']) is outside 0..$([int]$flash['inputports']-1)" }
        if ([int]$flash['channels'] -lt 2) { Bad "FLASH drive=1 needs at least 2 channels (In1 score + In2 clock)" }
    }
    if ($flash.ContainsKey('swfpath')) {
        $sp = $flash['swfpath']
        if (Test-Path $sp) { Ok "swf exists: $sp" } else { Bad "swf not found: $sp" }
    }
    foreach ($i in 3,4,5) {
        $th = Get-Props $objects[$i]
        if ($th.ContainsKey('truemode') -and [int]$th['truemode'] -ne 1) { Bad "THRESHOLD #$i truemode=$($th['truemode']), expected 1" }
        # a threshold that feeds an AND gate must emit INVALID_VALUE on fail
        # (falsemode 0), otherwise the AND object can never be false
        if ($th.ContainsKey('falsemode') -and [int]$th['falsemode'] -ne 0) { Bad "THRESHOLD #$i falsemode=$($th['falsemode']), expected 0 (INVALID_VALUE on fail)" }
        if ($th.ContainsKey('and/or') -and [int]$th['and/or'] -ne 1) { Bad "THRESHOLD #$i and/or=$($th['and/or']), expected 1" }
        if ($th.ContainsKey('to-input') -and $th.ContainsKey('inport1max')) {
            if ([double]$th['to-input'] -gt [double]$th['inport1max']) {
                Bad "THRESHOLD #$i to-input=$($th['to-input']) is above inport1max=$($th['inport1max'])"
            }
        }
    }
}

Write-Host ""
if ($fail -eq 0) { Write-Host "RESULT: OK - no problems found" -ForegroundColor Green; exit 0 }
Write-Host "RESULT: $fail problem(s) found" -ForegroundColor Red
exit 1
