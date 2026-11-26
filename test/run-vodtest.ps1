# Twitch VOD track test against a local ffmpeg RTMP listener.
#
# Two looping audio sources:
#   "Music": continuous 1000 Hz tone, mixer track 1 only (live only)
#   "Voice": 440 Hz beep 0.2 s every second, mixer tracks 1 and 2
# Profile "VodTest": Advanced output, stream track 1, Twitch VOD track 2.
# ffmpeg records every audio stream it receives; analyze-vodtest.py checks
# that stream #1 (the VOD track) has the voice but never the music, that the
# beeps line up across tracks, and (with -Dump) that both tracks go silent
# during the dump.
#
#   -Mode DumpDelay : dump_delay_service (the plugin)
#   -Mode Reference : stock rtmp_output via rtmp_custom; needs
#                     [General] EnableCustomServerVodTrack=true in user.ini
#
# Needs obs-websocket enabled and OBS running. Leaves the user's profile,
# collection and service as they were.

param(
	[ValidateSet('DumpDelay', 'Reference')][string]$Mode = 'DumpDelay',
	[string]$OutDir = (Join-Path $PSScriptRoot '..\test-output'),
	[int]$DelaySec = 10,
	[int]$RunSec = 70,
	[int]$DumpAt = 25,
	# turn the profile's VOD track off (the dock should warn)
	[switch]$NoVod,
	# save a screenshot of the screen (dock included) while live
	[switch]$Screenshot
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'obsws.ps1')

$GREEN = 4278255360
$BLUE = 4294901760
$listen = 'rtmp://127.0.0.1:1935/live'
$key = 'test'
$ffmpeg = (Get-Command ffmpeg).Source

New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$tag = $Mode.ToLower()
$recording = Join-Path $OutDir "vod-$tag.mkv"
$eventsFile = Join-Path $OutDir "vod-$tag-events.json"
Remove-Item $recording, $eventsFile -ErrorAction SilentlyContinue

# test tones
$music = Join-Path $OutDir 'tone-music-1000.wav'
$voice = Join-Path $OutDir 'tone-voice-440-beeps.wav'
if (-not (Test-Path $music)) {
	& $ffmpeg -hide_banner -loglevel error -f lavfi -i 'sine=f=1000:d=10:r=48000' -ac 2 -y $music
}
if (-not (Test-Path $voice)) {
	& $ffmpeg -hide_banner -loglevel error -f lavfi -i 'sine=f=440:d=10:r=48000' `
		-af "volume='if(lt(mod(t,1),0.2),1,0)':eval=frame" -ac 2 -y $voice
}

$wsConfig = Get-Content "$env:APPDATA\obs-studio\plugin_config\obs-websocket\config.json" -Raw | ConvertFrom-Json
$ws = Connect-Obs "ws://127.0.0.1:$($wsConfig.server_port)" $wsConfig.server_password

$origProfile = (Invoke-Obs $ws 'GetProfileList').currentProfileName
$origCollection = (Invoke-Obs $ws 'GetSceneCollectionList').currentSceneCollectionName
Write-Host "original profile '$origProfile', collection '$origCollection'"

$events = [ordered]@{ mode = $Mode }
$sw = $null
function Mark($name) { $script:events[$name] = [Math]::Round($script:sw.Elapsed.TotalSeconds, 3); Write-Host ("{0,7:N2}s  {1}" -f $script:events[$name], $name) }
function Set-Param($cat, $name, $value) {
	Invoke-Obs $ws 'SetProfileParameter' @{ parameterCategory = $cat; parameterName = $name; parameterValue = "$value" } | Out-Null
}

$recorder = $null
try {
	# --- profile: Advanced output, stream track 1, VOD track 2 ---
	$profiles = (Invoke-Obs $ws 'GetProfileList').profiles
	if ($profiles -notcontains 'VodTest') { Invoke-Obs $ws 'CreateProfile' @{ profileName = 'VodTest' } | Out-Null }
	Invoke-Obs $ws 'SetCurrentProfile' @{ profileName = 'VodTest' } | Out-Null
	Set-Param 'Output' 'Mode' 'Advanced'
	Set-Param 'Output' 'DelayEnable' 'false'
	Set-Param 'AdvOut' 'Encoder' 'obs_x264'
	Set-Param 'AdvOut' 'TrackIndex' 1
	Set-Param 'AdvOut' 'VodTrackEnabled' ($NoVod ? 'false' : 'true')
	Set-Param 'AdvOut' 'VodTrackIndex' 2
	# reload so the output handler is rebuilt in Advanced mode
	Invoke-Obs $ws 'SetCurrentProfile' @{ profileName = $origProfile } | Out-Null
	Invoke-Obs $ws 'SetCurrentProfile' @{ profileName = 'VodTest' } | Out-Null

	# --- scenes ---
	$collections = (Invoke-Obs $ws 'GetSceneCollectionList').sceneCollections
	if ($collections -contains 'VodTest') {
		Invoke-Obs $ws 'SetCurrentSceneCollection' @{ sceneCollectionName = 'VodTest' } | Out-Null
	} else {
		Invoke-Obs $ws 'CreateSceneCollection' @{ sceneCollectionName = 'VodTest' } | Out-Null
	}
	$video = Invoke-Obs $ws 'GetVideoSettings'
	$size = @{ width = $video.baseWidth; height = $video.baseHeight }
	$scenes = (Invoke-Obs $ws 'GetSceneList').scenes.sceneName
	foreach ($s in @(@{ scene = 'Live'; input = 'LiveColor'; color = $GREEN }, @{ scene = 'Censored'; input = 'SlateColor'; color = $BLUE })) {
		if ($scenes -notcontains $s.scene) {
			Invoke-Obs $ws 'CreateScene' @{ sceneName = $s.scene } | Out-Null
			Invoke-Obs $ws 'CreateInput' @{ sceneName = $s.scene; inputName = $s.input; inputKind = 'color_source_v3'; inputSettings = $size + @{ color = $s.color } } | Out-Null
		}
	}
	$inputs = (Invoke-Obs $ws 'GetInputList').inputs.inputName
	foreach ($a in @(@{ name = 'Music'; file = $music; tracks = @{ '1' = $true; '2' = $false; '3' = $false; '4' = $false; '5' = $false; '6' = $false } },
			 @{ name = 'Voice'; file = $voice; tracks = @{ '1' = $true; '2' = $true; '3' = $false; '4' = $false; '5' = $false; '6' = $false } })) {
		$settings = @{ local_file = $a.file; looping = $true; restart_on_activate = $false; close_when_inactive = $false }
		if ($inputs -notcontains $a.name) {
			Invoke-Obs $ws 'CreateInput' @{ sceneName = 'Live'; inputName = $a.name; inputKind = 'ffmpeg_source'; inputSettings = $settings } | Out-Null
		} else {
			Invoke-Obs $ws 'SetInputSettings' @{ inputName = $a.name; inputSettings = $settings } | Out-Null
		}
		Invoke-Obs $ws 'SetInputAudioTracks' @{ inputName = $a.name; inputAudioTracks = $a.tracks } | Out-Null
	}
	# keep the PC's own desktop/mic audio out of the test
	$special = Invoke-Obs $ws 'GetSpecialInputs'
	foreach ($p in $special.PSObject.Properties) {
		if ($p.Value) { try { Invoke-Obs $ws 'SetInputMute' @{ inputName = $p.Value; inputMuted = $true } | Out-Null } catch {} }
	}
	Invoke-Obs $ws 'SetCurrentProgramScene' @{ sceneName = 'Live' } | Out-Null

	# --- service ---
	if ($Mode -eq 'DumpDelay') {
		Invoke-Obs $ws 'SetStreamServiceSettings' @{
			streamServiceType = 'dump_delay_service'
			streamServiceSettings = @{
				server = $listen; key = $key; delay_sec = $DelaySec; slate_scene = 'Censored'; cut_margin_ms = 1000
				prev_type = 'rtmp_common'; prev_settings = @{ service = 'Twitch'; server = 'auto'; key = '' }
			}
		} | Out-Null
	} else {
		Invoke-Obs $ws 'SetStreamServiceSettings' @{
			streamServiceType = 'rtmp_custom'
			streamServiceSettings = @{ server = $listen; key = $key }
		} | Out-Null
	}
	$svc = Invoke-Obs $ws 'GetStreamServiceSettings'
	$events.service_type = $svc.streamServiceType
	$events.service_name = $svc.streamServiceSettings.service

	# --- run ---
	$recorder = Start-Process -FilePath $ffmpeg -PassThru -WindowStyle Hidden `
		-RedirectStandardError (Join-Path $OutDir "vod-$tag-ffmpeg.log") `
		-ArgumentList '-hide_banner', '-loglevel', 'info', '-listen', '1', '-i', "$listen/$key", '-map', '0', '-c', 'copy', '-f', 'matroska', '-y', "`"$recording`""
	Start-Sleep -Seconds 1

	$sw = [Diagnostics.Stopwatch]::StartNew()
	Invoke-Obs $ws 'StartStream' | Out-Null
	Mark 'stream_start'

	if ($Screenshot) {
		Start-Sleep -Seconds 3
		Add-Type -AssemblyName System.Windows.Forms, System.Drawing
		$bounds = [Windows.Forms.Screen]::PrimaryScreen.Bounds
		$bmp = [Drawing.Bitmap]::new($bounds.Width, $bounds.Height)
		[Drawing.Graphics]::FromImage($bmp).CopyFromScreen($bounds.Location, [Drawing.Point]::Empty, $bounds.Size)
		$shot = Join-Path $OutDir "vod-$tag$($NoVod ? '-novod' : '').png"
		$bmp.Save($shot, [Drawing.Imaging.ImageFormat]::Png)
		$bmp.Dispose()
		Mark 'screenshot'
	}

	if ($Mode -eq 'DumpDelay') {
		while ($sw.Elapsed.TotalSeconds -lt $DumpAt) { Start-Sleep -Milliseconds 50 }
		Invoke-Obs $ws 'TriggerHotkeyByName' @{ hotkeyName = 'dump_delay.dump' } | Out-Null
		Mark 'dump'
	}

	$lastScene = ''
	while ($sw.Elapsed.TotalSeconds -lt $RunSec) {
		$scene = (Invoke-Obs $ws 'GetCurrentProgramScene').currentProgramSceneName
		if ($scene -ne $lastScene) { Mark "program_scene=$scene"; $lastScene = $scene }
		Start-Sleep -Milliseconds 200
	}
	$status = Invoke-Obs $ws 'GetStreamStatus'
	$events.stream_active_before_stop = $status.outputActive
	$events.output_skipped_frames = $status.outputSkippedFrames
	Invoke-Obs $ws 'StopStream' | Out-Null
	Mark 'stream_stop'
}
finally {
	if ($recorder) {
		if (-not $recorder.WaitForExit(20000)) { Stop-Process -Id $recorder.Id -Force }
	}
	try { Invoke-Obs $ws 'StopStream' | Out-Null } catch {}
	Start-Sleep -Seconds 2
	try {
		Invoke-Obs $ws 'SetCurrentSceneCollection' @{ sceneCollectionName = $origCollection } | Out-Null
		Invoke-Obs $ws 'SetCurrentProfile' @{ profileName = $origProfile } | Out-Null
		Write-Host "restored profile '$origProfile', collection '$origCollection'"
	} catch { Write-Warning "restore failed: $_" }
	$events | ConvertTo-Json | Set-Content $eventsFile
}
