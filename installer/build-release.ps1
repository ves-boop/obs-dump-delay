# Build all release artifacts into release\:
#   obs-dump-delay-<ver>-windows-x64.zip          plugin folder for manual install
#   obs-dump-delay-<ver>-windows-x64-setup.exe    Inno Setup installer
#   obs-dump-delay.<ver>.nupkg                    Chocolatey package
#   SHA256SUMS.txt
#
# Needs: the CMake preset already configured, Inno Setup 6, Chocolatey.

param(
	[string]$Preset = 'windows-x64-local',
	[string]$RepoUrl = 'https://github.com/ves-boop/obs-dump-delay'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$release = Join-Path $root 'release'
$stage = Join-Path $release 'stage'
$iscc = 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
$cmake = 'C:\Program Files\CMake\bin\cmake.exe'
$choco = 'C:\ProgramData\chocolatey\bin\choco.exe'

function Invoke-Checked([string]$exe, [string[]]$argList) {
	& $exe @argList
	if ($LASTEXITCODE) { throw "$exe failed with exit code $LASTEXITCODE" }
}

$spec = Get-Content (Join-Path $root 'buildspec.json') -Raw | ConvertFrom-Json
$version = $spec.version
$obsMajor = [int]($spec.dependencies.'obs-studio'.version -split '\.')[0]
$repoRaw = $RepoUrl -replace '^https://github.com/', 'https://raw.githubusercontent.com/'
$repoRaw += '/main'
Write-Host "Dump Delay $version for OBS $obsMajor"

if (Test-Path $release) { Remove-Item -Recurse -Force $release }
New-Item -ItemType Directory -Force $stage | Out-Null

# 1. build and stage the plugin folder (no .pdb in the release)
Push-Location $root
try {
	Invoke-Checked $cmake @('--build', '--preset', $Preset)
	Invoke-Checked $cmake @('--install', 'build_x64', '--config', 'RelWithDebInfo', '--prefix', $stage)
} finally { Pop-Location }
$plugin = Join-Path $stage 'obs-dump-delay'
Remove-Item (Join-Path $plugin '*.pdb') -ErrorAction SilentlyContinue

# 2. zip: extract into C:\ProgramData\obs-studio\plugins
$zip = Join-Path $release "obs-dump-delay-$version-windows-x64.zip"
Compress-Archive -Path $plugin -DestinationPath $zip

# 3. installer
Invoke-Checked $iscc @('/Q', "/DAppVersion=$version", "/DObsMajor=$obsMajor", "/DStageDir=$plugin",
	"/DRepoUrl=$RepoUrl", "/DOutputDir=$release", (Join-Path $PSScriptRoot 'obs-dump-delay.iss'))
$setup = Get-Item (Join-Path $release "obs-dump-delay-$version-windows-x64-setup.exe")

# 4. chocolatey package, embedding the installer
$pkg = Join-Path $release 'choco'
Copy-Item -Recurse (Join-Path $root 'choco') $pkg
$tools = Join-Path $pkg 'tools'
Copy-Item $setup.FullName $tools
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $tools 'LICENSE.txt')

$fill = @{
	'@VERSION@' = $version; '@OBS_MAJOR@' = "$obsMajor"; '@OBS_NEXT_MAJOR@' = "$($obsMajor + 1)"
	'@REPO_URL@' = $RepoUrl; '@REPO_RAW@' = $repoRaw
	'@RELEASE_URL@' = "$RepoUrl/releases/tag/v$version"; '@SETUP_NAME@' = $setup.Name
	'@SETUP_SHA256@' = (Get-FileHash $setup.FullName -Algorithm SHA256).Hash
}
foreach ($f in @((Join-Path $pkg 'obs-dump-delay.nuspec'), (Join-Path $tools 'VERIFICATION.txt.in'))) {
	$text = Get-Content $f -Raw
	foreach ($k in $fill.Keys) { $text = $text.Replace($k, $fill[$k]) }
	Set-Content ($f -replace '\.in$', '') $text -NoNewline -Encoding utf8NoBOM
}
Remove-Item (Join-Path $tools 'VERIFICATION.txt.in')
Invoke-Checked $choco @('pack', (Join-Path $pkg 'obs-dump-delay.nuspec'), '--outputdirectory', $release, '--limit-output')

# 5. checksums
Remove-Item -Recurse -Force $stage, $pkg
$sums = Get-ChildItem $release -File | Sort-Object Name |
	ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name }
Set-Content (Join-Path $release 'SHA256SUMS.txt') $sums -Encoding utf8NoBOM

Get-ChildItem $release -File | Select-Object Name, @{ n = 'KB'; e = { [math]::Round($_.Length / 1KB) } }
