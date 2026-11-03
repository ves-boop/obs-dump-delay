# Point streaming at the dump-delay service with the local MediaMTX target
# (undo with restore-obs.ps1).
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'obsws.ps1')

$wsConfig = Get-Content "$env:APPDATA\obs-studio\plugin_config\obs-websocket\config.json" -Raw | ConvertFrom-Json
$ws = Connect-Obs "ws://127.0.0.1:$($wsConfig.server_port)" $wsConfig.server_password

$orig = Invoke-Obs $ws 'GetStreamServiceSettings'
Invoke-Obs $ws 'SetStreamServiceSettings' @{
	streamServiceType = 'dump_delay_service'
	streamServiceSettings = @{
		server = 'rtmp://127.0.0.1:1935/live'; key = 'test'
		prev_type = $orig.streamServiceType; prev_settings = $orig.streamServiceSettings
	}
} | Out-Null
"service: $((Invoke-Obs $ws 'GetStreamServiceSettings').streamServiceType)"
