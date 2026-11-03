# End-to-end dump test against a local MediaMTX (rtmp://127.0.0.1:1935).
#
# Live scene is a GREEN color source. At +20 s it turns RED ("forbidden"),
# at +25 s the dump hotkey is triggered, at +26 s the live source goes back
# to GREEN. The slate scene "Censored" is BLUE. ffmpeg records what a viewer
# receives; analyze-dumptest.ps1 checks that no red frame made it out.

param(
	[string]$OutDir = (Join-Path $PSScriptRoot '..\test-output'),
	[int]$DelaySec = 10,
	[int]$RunSec = 80
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'obsws.ps1')

$GREEN = 4278255360  # 0xFF00FF00 (ABGR)
$RED = 4278190335    # 0xFF0000FF
$BLUE = 4294901760   # 0xFFFF0000
$url = 'rtmp://127.0.0.1:1935/live'
$key = 'test'

New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$recording = Join-Path $OutDir 'received.mkv'
$eventsFile = Join-Path $OutDir 'events.json'
Remove-Item $recording, $eventsFile -ErrorAction SilentlyContinue

$wsConfig = Get-Content "$env:APPDATA\obs-studio\plugin_config\obs-websocket\config.json" -Raw | ConvertFrom-Json
$ws = Connect-Obs "ws://127.0.0.1:$($wsConfig.server_port)" $wsConfig.server_password

$origCollection = (Invoke-Obs $ws 'GetSceneCollectionList').currentSceneCollectionName
$origService = Invoke-Obs $ws 'GetStreamServiceSettings'
Write-Host "original collection '$origCollection', service '$($origService.streamServiceType)'"

$events = [ordered]@{}
$sw = $null
function Mark($name) { $script:events[$name] = [Math]::Round($script:sw.Elapsed.TotalSeconds, 3); Write-Host ("{0,7:N2}s  {1}" -f $script:events[$name], $name) }

$recorder = $null
try {
	$collections = (Invoke-Obs $ws 'GetSceneCollectionList').sceneCollections
	if ($collections -contains 'DumpDelayTest') {
		Invoke-Obs $ws 'SetCurrentSceneCollection' @{ sceneCollectionName = 'DumpDelayTest' } | Out-Null
	} else {
		Invoke-Obs $ws 'CreateSceneCollection' @{ sceneCollectionName = 'DumpDelayTest' } | Out-Null
	}

	$scenes = (Invoke-Obs $ws 'GetSceneList').scenes.sceneName
	$video = Invoke-Obs $ws 'GetVideoSettings'
	$size = @{ width = $video.baseWidth; height = $video.baseHeight }

	foreach ($s in @(@{ scene = 'Live'; input = 'LiveColor'; color = $GREEN }, @{ scene = 'Censored'; input = 'SlateColor'; color = $BLUE })) {
		if ($scenes -notcontains $s.scene) {
			Invoke-Obs $ws 'CreateScene' @{ sceneName = $s.scene } | Out-Null
			Invoke-Obs $ws 'CreateInput' @{ sceneName = $s.scene; inputName = $s.input; inputKind = 'color_source_v3'; inputSettings = $size + @{ color = $s.color } } | Out-Null
		} else {
			Invoke-Obs $ws 'SetInputSettings' @{ inputName = $s.input; inputSettings = $size + @{ color = $s.color } } | Out-Null
		}
	}
	Invoke-Obs $ws 'SetCurrentProgramScene' @{ sceneName = 'Live' } | Out-Null

	Invoke-Obs $ws 'SetStreamServiceSettings' @{
		streamServiceType = 'dump_delay_service'
		streamServiceSettings = @{
			server = $url; key = $key; delay_sec = $DelaySec; slate_scene = 'Censored'; cut_margin_ms = 1000
			prev_type = $origService.streamServiceType; prev_settings = $origService.streamServiceSettings
		}
	} | Out-Null

	$sw = [Diagnostics.Stopwatch]::StartNew()
	Invoke-Obs $ws 'StartStream' | Out-Null
	Mark 'stream_start'

	# The plugin connects only shortly before the first packet is due, so
	# retry until the stream exists on the server.
	do {
		Start-Sleep -Milliseconds 500
		$recorder = Start-Process -FilePath 'C:\ProgramData\chocolatey\bin\ffmpeg.exe' -PassThru -WindowStyle Hidden `
			-RedirectStandardError (Join-Path $OutDir 'ffmpeg-recorder.log') `
			-ArgumentList '-hide_banner', '-loglevel', 'warning', '-i', "$url/$key", '-c', 'copy', '-f', 'matroska', '-y', "`"$recording`""
		$exited = $recorder.WaitForExit(1500)
	} while ($exited -and $sw.Elapsed.TotalSeconds -lt 18)
	if ($exited) { throw 'recorder could not attach to the stream' }
	Mark 'recorder_start'

	while ($sw.Elapsed.TotalSeconds -lt 20) { Start-Sleep -Milliseconds 50 }
	Invoke-Obs $ws 'SetInputSettings' @{ inputName = 'LiveColor'; inputSettings = @{ color = $RED } } | Out-Null
	Mark 'red_on'

	while ($sw.Elapsed.TotalSeconds -lt 25) { Start-Sleep -Milliseconds 50 }
	Invoke-Obs $ws 'TriggerHotkeyByName' @{ hotkeyName = 'dump_delay.dump' } | Out-Null
	Mark 'dump'

	while ($sw.Elapsed.TotalSeconds -lt 26) { Start-Sleep -Milliseconds 50 }
	Invoke-Obs $ws 'SetInputSettings' @{ inputName = 'LiveColor'; inputSettings = @{ color = $GREEN } } | Out-Null
	Mark 'red_off'

	$lastScene = ''
	while ($sw.Elapsed.TotalSeconds -lt $RunSec) {
		$scene = (Invoke-Obs $ws 'GetCurrentProgramScene').currentProgramSceneName
		if ($scene -ne $lastScene) { Mark "program_scene=$scene"; $lastScene = $scene }
		Start-Sleep -Milliseconds 200
	}

	$status = Invoke-Obs $ws 'GetStreamStatus'
	$events.stream_active_before_stop = $status.outputActive
	$events.output_bytes = $status.outputBytes
	Invoke-Obs $ws 'StopStream' | Out-Null
	Mark 'stream_stop'
}
finally {
	if ($recorder) {
		if (-not $recorder.WaitForExit(20000)) { Stop-Process -Id $recorder.Id -Force }
	}
	try { Invoke-Obs $ws 'StopStream' | Out-Null } catch {}
	Start-Sleep -Seconds 2

	# obs-websocket rejects empty settings; a fresh profile's rtmp_common has
	# none, which OBS shows as Twitch / Auto with no key.
	$restoreSettings = $origService.streamServiceSettings
	if (-not $restoreSettings -or -not @($restoreSettings.PSObject.Properties).Count) {
		$restoreSettings = @{ service = 'Twitch'; server = 'auto'; key = '' }
	}
	try {
		Invoke-Obs $ws 'SetStreamServiceSettings' @{
			streamServiceType = $origService.streamServiceType
			streamServiceSettings = $restoreSettings
		} | Out-Null
		Write-Host "restored service '$($origService.streamServiceType)'"
	} catch { Write-Warning "service not restored: $_" }
	try {
		Invoke-Obs $ws 'SetCurrentSceneCollection' @{ sceneCollectionName = $origCollection } | Out-Null
		Write-Host "restored collection '$origCollection'"
	} catch { Write-Warning "collection not restored: $_" }
	$events | ConvertTo-Json | Set-Content $eventsFile
}
