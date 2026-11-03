# Generate installer and README artwork from data/panicone.png (needs ffmpeg).
# Run once; the outputs are committed.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root 'data\panicone.png'
$assets = Join-Path $PSScriptRoot 'assets'
$ffmpeg = 'ffmpeg'
New-Item -ItemType Directory -Force $assets | Out-Null

function Invoke-Ffmpeg([string[]]$ArgList) {
	& $ffmpeg -hide_banner -loglevel error -y @ArgList
	if ($LASTEXITCODE) { throw "ffmpeg failed: $ArgList" }
}

# Icon: PNG-compressed entries (Vista+) packed into one .ico.
$sizes = 16, 24, 32, 48, 64, 128, 256
$images = foreach ($s in $sizes) {
	$tmp = Join-Path $env:TEMP "panicone-$s.png"
	Invoke-Ffmpeg @('-i', $src, '-vf', "scale=-1:${s}:flags=lanczos,pad=${s}:${s}:(ow-iw)/2:0:color=0x00000000", '-pix_fmt', 'rgba', $tmp)
	$bytes = [IO.File]::ReadAllBytes($tmp)
	Remove-Item $tmp
	[pscustomobject]@{ Size = $s; Bytes = $bytes }
}
$ms = [IO.MemoryStream]::new()
$w = [IO.BinaryWriter]::new($ms)
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($i in $images) {
	$dim = if ($i.Size -ge 256) { 0 } else { $i.Size }
	$w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
	$w.Write([uint16]1); $w.Write([uint16]32)
	$w.Write([uint32]$i.Bytes.Length); $w.Write([uint32]$offset)
	$offset += $i.Bytes.Length
}
foreach ($i in $images) { $w.Write($i.Bytes) }
[IO.File]::WriteAllBytes((Join-Path $assets 'panicone.ico'), $ms.ToArray())

# Wizard images: large (welcome page, opaque) and small (top right, alpha),
# at 100% and 200% scale.
foreach ($scale in 1, 2) {
	$suffix = if ($scale -eq 2) { '-2x' } else { '' }
	$lw = 164 * $scale; $lh = 314 * $scale; $bw = 136 * $scale
	Invoke-Ffmpeg @('-f', 'lavfi', '-i', "color=c=0x1d1b22:s=${lw}x${lh}", '-i', $src, '-frames:v', '1',
		'-filter_complex', "[1]scale=${bw}:-1:flags=lanczos[b];[0][b]overlay=(W-w)/2:(H-h)/2",
		'-pix_fmt', 'bgr24', (Join-Path $assets "wizard-large$suffix.bmp"))

	$ss = 55 * $scale
	Invoke-Ffmpeg @('-i', $src, '-vf', "scale=-1:${ss}:flags=lanczos,pad=${ss}:${ss}:(ow-iw)/2:0:color=0x00000000",
		'-pix_fmt', 'bgra', (Join-Path $assets "wizard-small$suffix.bmp"))
}

Get-ChildItem $assets | Select-Object Name, Length
