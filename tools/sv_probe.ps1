<#
  sv_probe.ps1 - verifies a BrainBay "SV-Server" object without any game.

  It joins the discovery group and, in parallel, connects to the data port, then
  decodes whatever arrives with the (still assumed) layout of sv_proto.h:

      message header : uint32 size, uint32 type
      CHANNEL_ADDED  : int32 id, double rate, ascii name
      SAMPLES        : int32 id, int32 count, double samples[count]
      ANNOUNCE       : uint32 protocol, ascii server name
      PLAY/PAUSE/STOP: header only

  Run it with the two test ports the SV-Server object was configured with, e.g.

      powershell -NoProfile -ExecutionPolicy Bypass -File tools\sv_probe.ps1 `
                 -McastPort 23900 -DataPort 23901 -Seconds 15

  (plain "& tools\sv_probe.ps1" fails with PSSecurityException - use -File)
#>
param(
	[int]$McastPort = 23900,
	[int]$DataPort  = 23901,
	[int]$Seconds   = 15,
	[string]$Group  = "239.192.100.200"
)

$ErrorActionPreference = "Continue"

# ---------------------------------------------------------------------------
#  the TCP side: connect, decode the channel list, the sample blocks and the
#  session state messages
# ---------------------------------------------------------------------------
$tcpJob = Start-Job -ArgumentList $DataPort, $Seconds -ScriptBlock {

	param($DataPort, $Seconds)

	function Decode([byte[]]$b, [int]$len)
	{
		$type = [BitConverter]::ToInt32($b, 4)

		switch ($type) {
			0 { $proto = [BitConverter]::ToUInt32($b, 8)
			    $name  = [Text.Encoding]::ASCII.GetString($b, 12, [Math]::Max(0, $len - 13))
			    "ANNOUNCE   protocol=$proto server='$name'" }
			1 { $id   = [BitConverter]::ToInt32($b, 8)
			    $rate = [BitConverter]::ToDouble($b, 12)
			    $name = [Text.Encoding]::ASCII.GetString($b, 20, [Math]::Max(0, $len - 21))
			    "CHANNEL_ADDED id=$id rate=$rate name='$name'" }
			2 { "CHANNEL_REMOVED id=$([BitConverter]::ToInt32($b,8))" }
			3 { $name = [Text.Encoding]::ASCII.GetString($b, 12, [Math]::Max(0, $len - 13))
			    "CHANNEL_NAME_CHANGED id=$([BitConverter]::ToInt32($b,8)) name='$name'" }
			4 { "CHANNEL_RATE_CHANGED id=$([BitConverter]::ToInt32($b,8)) rate=$([BitConverter]::ToDouble($b,12))" }
			5 { $id    = [BitConverter]::ToInt32($b, 8)
			    $count = [BitConverter]::ToInt32($b, 12)
			    $first = [BitConverter]::ToDouble($b, 16)
			    $last  = [BitConverter]::ToDouble($b, 16 + ($count - 1) * 8)
			    "SAMPLES    id=$id count=$count first=$([Math]::Round($first,4)) last=$([Math]::Round($last,4))" }
			6 { "PLAY" } 7 { "PAUSE" } 8 { "STOP" }
			default { "type=$type size=$len" }
		}
	}

	$client = New-Object Net.Sockets.TcpClient
	$client.ReceiveTimeout = 2000
	try { $client.Connect("127.0.0.1", $DataPort) }
	catch { "TCP connect to 127.0.0.1:$DataPort failed: $($_.Exception.Message)"; return }

	$stream = $client.GetStream()
	$buf    = New-Object byte[] 4096
	$fill   = 0
	$stop   = (Get-Date).AddSeconds($Seconds)

	while ((Get-Date) -lt $stop)
	{
		try { $n = $stream.Read($buf, $fill, $buf.Length - $fill) }
		catch { continue }

		if ($n -le 0) { "TCP closed by the server"; break }
		$fill += $n

		# drain all complete messages of the (assumed) size+type framing
		$pos = 0
		while ($fill - $pos -ge 8)
		{
			$size = [BitConverter]::ToInt32($buf, $pos)
			if ($size -lt 8 -or $size -gt 1024) { "bad size=$size - wrong layout?"; $pos = $fill; break }
			if ($fill - $pos -lt $size) { break }

			$view = New-Object byte[] $size
			[Array]::Copy($buf, $pos, $view, 0, $size)
			Decode $view $size
			$pos += $size
		}

		if ($pos -gt 0)
		{
			[Array]::Copy($buf, $pos, $buf, 0, $fill - $pos)
			$fill -= $pos
		}
	}

	$client.Close()
}

# ---------------------------------------------------------------------------
#  the discovery side: join the group and decode the announcements
# ---------------------------------------------------------------------------
"--- discovery: joining $Group`:$McastPort for $Seconds s ---"

$udp = New-Object Net.Sockets.UdpClient($McastPort)
$udp.Client.ReceiveTimeout = 1000
try   { $udp.JoinMulticastGroup([Net.IPAddress]::Parse($Group)) }
catch { "JoinMulticastGroup failed: $($_.Exception.Message)" }

$remote   = New-Object Net.IPEndPoint([Net.IPAddress]::Any, 0)
$stopUdp  = (Get-Date).AddSeconds($Seconds)
$lastBeat = $null
$beats    = 0

while ((Get-Date) -lt $stopUdp)
{
	try   { $data = $udp.Receive([ref]$remote) }
	catch { continue }                       # 1 s timeout, keep waiting

	$type = [BitConverter]::ToInt32($data, 4)
	$now  = Get-Date

	if ($type -eq 0)
	{
		$beats++
		$gap = if ($lastBeat) { ", gap $([Math]::Round(($now - $lastBeat).TotalMilliseconds)) ms" } else { "" }
		$lastBeat = $now
		$name = [Text.Encoding]::ASCII.GetString($data, 12, [Math]::Max(0, $data.Length - 13))
		"ANNOUNCE from $($remote.Address):$($remote.Port)  size=$($data.Length)  server='$name'$gap"
	}
	else
	{
		"packet from $($remote.Address) type=$type size=$($data.Length)"
	}
}

$udp.Close()

# ---------------------------------------------------------------------------
#  report
# ---------------------------------------------------------------------------
"--- data connection ---"
Wait-Job $tcpJob -Timeout 5 | Out-Null
Receive-Job $tcpJob
Remove-Job $tcpJob -Force

""
"--- summary ---"
"announcements received : $beats"
if ($beats -gt 0 -and $lastBeat) { "heartbeat looks alive (see the gaps above)" }
else { "no announcement: check sv_mcastport, the firewall, or that the session is running" }
