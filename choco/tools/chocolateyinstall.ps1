$ErrorActionPreference = 'Stop'

$toolsDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$installer = Get-ChildItem $toolsDir -Filter 'obs-dump-delay-*-setup.exe' | Select-Object -First 1

if (Get-Process obs64 -ErrorAction SilentlyContinue) {
	throw 'OBS Studio is running. Close it and run the install again.'
}

$packageArgs = @{
	packageName    = $env:ChocolateyPackageName
	fileType       = 'exe'
	file64         = $installer.FullName
	silentArgs     = '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /LOG'
	validExitCodes = @(0)
	softwareName   = 'Dump Delay for OBS Studio'
}
Install-ChocolateyInstallPackage @packageArgs

# the embedded installer isn't needed after installation
Remove-Item $installer.FullName -Force -ErrorAction SilentlyContinue
