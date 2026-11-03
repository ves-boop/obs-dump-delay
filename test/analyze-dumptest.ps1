# Analyze test-output\received.mkv from run-dumptest.ps1.

param([string]$OutDir = (Join-Path $PSScriptRoot '..\test-output'))

$ErrorActionPreference = 'Stop'
$recording = Join-Path $OutDir 'received.mkv'
$ffmpeg = 'C:\ProgramData\chocolatey\bin\ffmpeg.exe'
$ffprobe = 'C:\ProgramData\chocolatey\bin\ffprobe.exe'

# Per-frame plane means (Y U V) from showinfo.
$frames = & $ffmpeg -hide_banner -i $recording -an -vf showinfo -f null - 2>&1 |
	Select-String 'pts_time:\s*([-\d.]+).*?mean:\[(\d+) (\d+) (\d+)\]' |
	ForEach-Object {
		$g = $_.Matches[0].Groups
		$u = [int]$g[3].Value; $v = [int]$g[4].Value
		$color = if ($v -gt 200 -and $u -lt 128) { 'RED' }
			elseif ($u -gt 200 -and $v -lt 160) { 'BLUE' }
			elseif ($u -lt 100 -and $v -lt 100) { 'GREEN' }
			else { 'mixed' }
		[pscustomobject]@{ t = [double]$g[1].Value; color = $color; yuv = "$($g[2].Value) $u $v" }
	}

if (-not $frames) { throw 'no video frames decoded' }

# Color segments.
$segments = @()
$cur = $null
foreach ($f in $frames) {
	if (-not $cur -or $cur.color -ne $f.color) {
		$cur = [pscustomobject]@{ color = $f.color; start = $f.t; end = $f.t; frames = 0 }
		$segments += $cur
	}
	$cur.end = $f.t
	$cur.frames++
}

# Largest gaps between consecutive video frames.
$gaps = for ($i = 1; $i -lt $frames.Count; $i++) {
	[pscustomobject]@{ at = $frames[$i - 1].t; gap = [Math]::Round($frames[$i].t - $frames[$i - 1].t, 3) }
}
$nonMonotonic = @($gaps | Where-Object gap -le 0).Count

# Packet level: dts strictly increasing, no duplicate pts.
$vpk = & $ffprobe -v error -select_streams v:0 -show_entries packet=pts,dts -of csv=p=0 $recording |
	Where-Object { $_ -notmatch 'N/A' } |
	ForEach-Object { $p = $_ -split ','; [pscustomobject]@{ pts = [long]$p[0]; dts = [long]$p[1] } }
$dtsBad = 0
for ($i = 1; $i -lt $vpk.Count; $i++) { if ($vpk[$i].dts -le $vpk[$i - 1].dts) { $dtsBad++ } }
$ptsDup = $vpk.Count - @($vpk.pts | Sort-Object -Unique).Count

# Audio packet continuity.
$audio = & $ffprobe -v error -select_streams a:0 -show_entries packet=pts_time,duration_time -of csv=p=0 $recording |
	ForEach-Object { $p = $_ -split ','; [pscustomobject]@{ t = [double]$p[0]; d = [double]$p[1] } }
$audioGaps = for ($i = 1; $i -lt $audio.Count; $i++) {
	$expected = $audio[$i - 1].t + $audio[$i - 1].d
	$delta = [Math]::Round($audio[$i].t - $expected, 3)
	if ([Math]::Abs($delta) -gt 0.03) { [pscustomobject]@{ at = $audio[$i - 1].t; delta = $delta } }
}

Write-Host "== video: $($frames.Count) frames, $([Math]::Round($frames[0].t,2))s .. $([Math]::Round($frames[-1].t,2))s"
$segments | ForEach-Object {
	'{0,-6} {1,8:N2}s -> {2,8:N2}s  ({3,5:N2}s, {4} frames)' -f $_.color, $_.start, $_.end, ($_.end - $_.start), $_.frames
} | Write-Host
Write-Host "== largest video gaps:"
$gaps | Sort-Object gap -Descending | Select-Object -First 3 | ForEach-Object { '  {0,6:N3}s after t={1:N2}s' -f $_.gap, $_.at } | Write-Host
Write-Host "== non-monotonic decoded frame timestamps: $nonMonotonic"
Write-Host "== video packets: $($vpk.Count), non-increasing dts: $dtsBad, duplicate pts: $ptsDup"
Write-Host "== audio: $($audio.Count) packets, $([Math]::Round($audio[0].t,2))s .. $([Math]::Round($audio[-1].t,2))s, discontinuities > 30 ms: $(@($audioGaps).Count)"
$audioGaps | Select-Object -First 5 | ForEach-Object { '  {0:+0.000;-0.000}s at t={1:N2}s' -f $_.delta, $_.at } | Write-Host

$red = @($frames | Where-Object color -eq 'RED').Count
$order = ($segments | Where-Object color -ne 'mixed' | ForEach-Object color) -join ' '
Write-Host ''
Write-Host "RESULT: red frames received = $red; color order = $order"
if ($red -eq 0) { Write-Host 'PASS: censored content never reached the server' } else { Write-Host 'FAIL: red frames leaked' }
if ($nonMonotonic -eq 0 -and $dtsBad -eq 0 -and $ptsDup -eq 0 -and @($audioGaps).Count -eq 0) {
	Write-Host 'PASS: timestamps clean (video order, audio continuity)'
} else {
	Write-Host 'FAIL: timestamp problems, see above'
}
