$ErrorActionPreference = 'Stop'

if (Get-Process obs64 -ErrorAction SilentlyContinue) {
	throw 'OBS Studio is running. Close it and run the uninstall again.'
}

[array]$keys = Get-UninstallRegistryKey -SoftwareName 'Dump Delay for OBS Studio'
if ($keys.Count -ne 1) {
	Write-Warning "Expected one 'Dump Delay for OBS Studio' entry, found $($keys.Count); nothing uninstalled."
	return
}

$packageArgs = @{
	packageName    = $env:ChocolateyPackageName
	fileType       = 'exe'
	file           = ($keys[0].UninstallString -replace '"', '')
	silentArgs     = '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART'
	validExitCodes = @(0)
}
Uninstall-ChocolateyPackage @packageArgs
