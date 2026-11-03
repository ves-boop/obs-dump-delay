# Put OBS back on the user's collection and a normal service after a test run.
param([string]$Collection = 'Untitled')

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'obsws.ps1')

$wsConfig = Get-Content "$env:APPDATA\obs-studio\plugin_config\obs-websocket\config.json" -Raw | ConvertFrom-Json
$ws = Connect-Obs "ws://127.0.0.1:$($wsConfig.server_port)" $wsConfig.server_password

$service = Invoke-Obs $ws 'GetStreamServiceSettings'
if ($service.streamServiceType -eq 'dump_delay_service') {
	$prevType = $service.streamServiceSettings.prev_type
	$prev = $service.streamServiceSettings.prev_settings
	if (-not $prev -or -not @($prev.PSObject.Properties).Count) { $prev = @{ service = 'Twitch'; server = 'auto'; key = '' } }
	Invoke-Obs $ws 'SetStreamServiceSettings' @{ streamServiceType = ($prevType ? $prevType : 'rtmp_common'); streamServiceSettings = $prev } | Out-Null
}
Invoke-Obs $ws 'SetCurrentSceneCollection' @{ sceneCollectionName = $Collection } | Out-Null

$service = Invoke-Obs $ws 'GetStreamServiceSettings'
"service: $($service.streamServiceType) / $($service.streamServiceSettings.service)"
"collection: $((Invoke-Obs $ws 'GetSceneCollectionList').currentSceneCollectionName)"
