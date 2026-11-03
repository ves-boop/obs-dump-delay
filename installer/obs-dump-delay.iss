; Dump Delay for OBS Studio - Windows installer (Inno Setup 6)
; Copyright (C) 2026 Valerio Enrico Salvo (Ves)
; SPDX-License-Identifier: GPL-2.0-or-later
;
; Built by installer\build-release.ps1, which passes:
;   /DAppVersion=x.y.z  /DObsMajor=NN  /DStageDir=<folder with the plugin>
;   /DRepoUrl=https://github.com/...  /DOutputDir=<release folder>

#define AppName "Dump Delay for OBS Studio"
#define AppPublisher "Valerio Enrico Salvo (Ves)"

[Setup]
; Keep this AppId forever: it is how upgrades find the installed version.
AppId={{C998B18A-9B24-466A-B51B-7F47382B1258}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#RepoUrl}
AppSupportURL={#RepoUrl}/issues
AppUpdatesURL={#RepoUrl}/releases
AppCopyright=Copyright (C) 2026 {#AppPublisher}
VersionInfoVersion={#AppVersion}
VersionInfoDescription={#AppName} setup

; OBS's plugin folder (current layout: <plugins>\<name>\<name>.dll)
DefaultDirName={commonappdata}\obs-studio\plugins\obs-dump-delay
DisableDirPage=yes
DisableProgramGroupPage=yes
UninstallFilesDir={app}\uninstall
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\uninstall\panicone.ico
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; OBS must be closed; checked in [Code] with a clearer message
CloseApplications=no

LicenseFile=..\LICENSE
SetupIconFile=assets\panicone.ico
WizardStyle=modern
WizardImageFile=assets\wizard-large.bmp,assets\wizard-large-2x.bmp
WizardSmallImageFile=assets\wizard-small.bmp,assets\wizard-small-2x.bmp
WizardImageAlphaFormat=defined
Compression=lzma2/max
SolidCompression=yes
OutputDir={#OutputDir}
OutputBaseFilename=obs-dump-delay-{#AppVersion}-windows-x64-setup

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "it"; MessagesFile: "compiler:Languages\Italian.isl"

[CustomMessages]
en.ObsRunning=OBS Studio is running.%n%nClose OBS Studio, then click Retry.
it.ObsRunning=OBS Studio è in esecuzione.%n%nChiudi OBS Studio, poi fai clic su Riprova.
en.ObsNotFound=OBS Studio was not found on this computer.%n%nDump Delay is a plugin for OBS Studio {#ObsMajor}. Install it anyway?
it.ObsNotFound=OBS Studio non è stato trovato su questo computer.%n%nDump Delay è un plugin per OBS Studio {#ObsMajor}. Installarlo comunque?
en.ObsWrongVersion=This version of Dump Delay is built for OBS Studio {#ObsMajor}, but OBS Studio %1 is installed.%n%nIt may not load, or may make OBS crash. Install anyway?
it.ObsWrongVersion=Questa versione di Dump Delay è compilata per OBS Studio {#ObsMajor}, ma è installato OBS Studio %1.%n%nPotrebbe non caricarsi o far chiudere OBS in modo anomalo. Installarla comunque?

[Messages]
en.FinishedLabel=Setup has finished installing [name] on your computer.%n%nStart OBS Studio: the Dump Delay panel opens on the right. Open its Settings… for instructions.
it.FinishedLabel=L'installazione di [name] è stata completata.%n%nAvvia OBS Studio: il pannello Dump Delay si apre sulla destra. Apri le sue Impostazioni… per le istruzioni.

[InstallDelete]
; legacy plugin layout (bin\64bit) from early builds
Type: filesandordirs; Name: "{app}\bin"

[Files]
Source: "{#StageDir}\obs-dump-delay.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "assets\panicone.ico"; DestDir: "{app}\uninstall"; Flags: ignoreversion

[UninstallDelete]
Type: dirifempty; Name: "{app}\data\locale"
Type: dirifempty; Name: "{app}\data"
Type: dirifempty; Name: "{app}"

[Code]
function IsObsRunning(): Boolean;
var
  Locator, Service, Processes: Variant;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Processes := Service.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = ''obs64.exe''');
    Result := Processes.Count > 0;
  except
    Log('Could not query running processes: ' + GetExceptionMessage);
  end;
end;

function WaitForObsClosed(): Boolean;
begin
  Result := True;
  while IsObsRunning() do
    if SuppressibleMsgBox(CustomMessage('ObsRunning'), mbError, MB_RETRYCANCEL, IDCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
end;

function ObsExePath(): String;
var
  InstallDir: String;
begin
  if not RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', InstallDir) then
    InstallDir := ExpandConstant('{commonpf64}\obs-studio');
  Result := AddBackslash(InstallDir) + 'bin\64bit\obs64.exe';
end;

{ The plugin links against the FFmpeg and Qt builds shipped with one OBS
  major version: refuse a mismatch unless the user insists (silent installs,
  e.g. Chocolatey, take the default answer: No). }
function CheckObsVersion(): Boolean;
var
  Exe, Version, Major: String;
begin
  Result := True;
  Exe := ObsExePath();
  if not FileExists(Exe) then
  begin
    Result := SuppressibleMsgBox(CustomMessage('ObsNotFound'), mbConfirmation, MB_YESNO, IDNO) = IDYES;
    Exit;
  end;
  if not GetVersionNumbersString(Exe, Version) then
    Exit;
  Major := Copy(Version, 1, Pos('.', Version) - 1);
  Log('Found OBS Studio ' + Version + ' at ' + Exe);
  if Major <> '{#ObsMajor}' then
    Result := SuppressibleMsgBox(FmtMessage(CustomMessage('ObsWrongVersion'), [Version]),
      mbConfirmation, MB_YESNO, IDNO) = IDYES;
end;

function InitializeSetup(): Boolean;
begin
  Result := CheckObsVersion() and WaitForObsClosed();
end;

function InitializeUninstall(): Boolean;
begin
  Result := WaitForObsClosed();
end;
