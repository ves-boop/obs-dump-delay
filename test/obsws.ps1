# Minimal obs-websocket v5 client (dot-source this file).

function Send-ObsMessage($ws, $obj) {
	$json = $obj | ConvertTo-Json -Depth 30 -Compress
	$bytes = [Text.Encoding]::UTF8.GetBytes($json)
	$null = $ws.SendAsync([ArraySegment[byte]]::new($bytes), 'Text', $true,
		[Threading.CancellationToken]::None).GetAwaiter().GetResult()
}

function Receive-ObsMessage($ws) {
	$buf = [byte[]]::new(65536)
	$ms = [IO.MemoryStream]::new()
	do {
		$r = $ws.ReceiveAsync([ArraySegment[byte]]::new($buf),
			[Threading.CancellationToken]::None).GetAwaiter().GetResult()
		$ms.Write($buf, 0, $r.Count)
	} while (-not $r.EndOfMessage)
	[Text.Encoding]::UTF8.GetString($ms.ToArray()) | ConvertFrom-Json -Depth 50
}

function Connect-Obs([string]$Uri, [string]$Password) {
	$ws = [System.Net.WebSockets.ClientWebSocket]::new()
	$null = $ws.ConnectAsync([uri]$Uri, [Threading.CancellationToken]::None).GetAwaiter().GetResult()

	$hello = Receive-ObsMessage $ws
	$identify = @{ rpcVersion = 1; eventSubscriptions = 0 }
	if ($hello.d.authentication) {
		$sha = [Security.Cryptography.SHA256]::Create()
		$utf8 = [Text.Encoding]::UTF8
		$secret = [Convert]::ToBase64String($sha.ComputeHash($utf8.GetBytes($Password + $hello.d.authentication.salt)))
		$identify.authentication = [Convert]::ToBase64String($sha.ComputeHash($utf8.GetBytes($secret + $hello.d.authentication.challenge)))
	}
	Send-ObsMessage $ws @{ op = 1; d = $identify }

	$identified = Receive-ObsMessage $ws
	if ($identified.op -ne 2) { throw "obs-websocket identify failed: $($identified | ConvertTo-Json -Compress)" }
	$ws
}

function Invoke-Obs($ws, [string]$Type, $Data = @{}) {
	$id = [guid]::NewGuid().ToString()
	Send-ObsMessage $ws @{ op = 6; d = @{ requestType = $Type; requestId = $id; requestData = $Data } }
	while ($true) {
		$m = Receive-ObsMessage $ws
		if ($m.op -eq 7 -and $m.d.requestId -eq $id) {
			if (-not $m.d.requestStatus.result) {
				throw "$Type failed: $($m.d.requestStatus.code) $($m.d.requestStatus.comment)"
			}
			return $m.d.responseData
		}
	}
}
