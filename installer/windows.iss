; PackageDir, OutputDir and AppVersion are supplied by package-windows.ps1.
#ifndef AppVersion
  #error AppVersion must come from VERSION
#endif
[Setup]
AppId={{D40BB76C-E0EB-48AD-94BD-A6C55D883D64}
AppName=Bokis Twitch Chat Plugin
AppVersion={#AppVersion}
AppPublisher=Boki
AppPublisherURL=https://github.com/BokiBokiPixelDeveloper/boki-twitch-chat-plugin
DefaultDirName={userappdata}\obs-studio\plugins\bokis-twitch-chat-plugin
DisableDirPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
OutputDir={#OutputDir}
OutputBaseFilename=bokis-twitch-chat-plugin-setup-{#AppVersion}-windows-x86_64
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=Bokis Twitch Chat Plugin
UninstallDisplayIcon={app}\bin\64bit\bokis-twitch-chat-plugin.dll
CloseApplications=no
RestartApplications=no
ChangesEnvironment=yes
InfoBeforeFile={#PackageDir}\INSTALL_WINDOWS.md
LicenseFile={#PackageDir}\LICENSE
SetupLogging=yes

[Files]
Source: "{#PackageDir}\plugin\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PackageDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PackageDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PackageDir}\INSTALL_WINDOWS.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PackageDir}\VERSION"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PackageDir}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs

[Code]
const
  InvalidHandle = -1;
var
  UpdateLock, UseLock: THandle;
  DiscoveryDirectory: string;

function CreateFileW(Name: string; Access, Sharing: Cardinal; Security: Integer;
  Creation, Attributes: Cardinal; Template: THandle): THandle;
  external 'CreateFileW@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';
function GetShortPathNameW(LongPath, ShortPath: string; Size: Cardinal): Cardinal;
  external 'GetShortPathNameW@kernel32.dll stdcall';

function IsAscii(Value: string): Boolean;
var I: Integer;
begin
  Result := True;
  for I := 1 to Length(Value) do
    if Ord(Value[I]) > 127 then begin Result := False; exit; end;
end;

function ResolveDiscoveryDirectory: Boolean;
var Buffer: string; Count: Cardinal;
begin
  DiscoveryDirectory := ExtractFileDir(ExpandConstant('{app}'));
  if not IsAscii(DiscoveryDirectory) then begin
    ForceDirectories(DiscoveryDirectory);
    SetLength(Buffer, 32768);
    Count := GetShortPathNameW(DiscoveryDirectory, Buffer, 32768);
    if (Count = 0) or (Count >= 32768) then begin Result := False; exit; end;
    SetLength(Buffer, Count);
    DiscoveryDirectory := Buffer;
  end;
  Result := IsAscii(DiscoveryDirectory);
end;

function RegisterEnvironment: Boolean;
begin
  { CI uses a disposable destination without changing OBS discovery. }
  Result := ExpandConstant('{param:RegisterEnv|1}') <> '0';
end;

function PendingPath: string;
begin
  { Override also keeps CI away from the real user's updater state. }
  Result := ExpandConstant('{param:StateRoot|{localappdata}\BokisTwitchChatPlugin}') + '\cache\pending';
end;

function BinSearchPath: string;
begin
  Result := DiscoveryDirectory + '\%module%\bin\64bit';
end;

function DataSearchPath: string;
begin
  { OBS appends /%module%. The plugin's resources are embedded in its DLL. }
  Result := DiscoveryDirectory;
end;

function EnvironmentCompatible(Name, Expected: string): Boolean;
var Existing: string;
begin
  Result := not RegQueryStringValue(HKCU, 'Environment', Name, Existing);
  if not Result then Result := (Existing = '') or (CompareText(Existing, Expected) = 0);
  { A machine-level custom path must not be silently hidden either. }
  if Result and RegQueryStringValue(HKLM,
    'SYSTEM\CurrentControlSet\Control\Session Manager\Environment', Name, Existing) then
    Result := (Existing = '') or (CompareText(Existing, Expected) = 0);
end;

function WritableBinary(Name: string): Boolean;
var Handle: THandle;
begin
  Result := True;
  if not FileExists(Name) then exit;
  { An image section or an open file denies this write/exclusive open. }
  Handle := CreateFileW(Name, $C0000000, 0, 0, 3, $80, 0);
  Result := Handle <> InvalidHandle;
  if Result then CloseHandle(Handle);
end;

procedure ReleaseLocks;
begin
  if UseLock <> InvalidHandle then CloseHandle(UseLock);
  if UpdateLock <> InvalidHandle then CloseHandle(UpdateLock);
  UseLock := InvalidHandle;
  UpdateLock := InvalidHandle;
end;

function LockInstallation: string;
var BinDir: string;
begin
  Result := '';
  ReleaseLocks;
  ForceDirectories(PendingPath);
  UpdateLock := CreateFileW(PendingPath + '\update.lock', $C0000000, 0, 0, 4, $80, 0);
  if UpdateLock = InvalidHandle then begin
    Result := 'An updater or another installer is active. Wait for it to finish.';
    exit;
  end;
  if FileExists(PendingPath + '\pending.json') or FileExists(PendingPath + '\transaction.json') then begin
    Result := 'A pending update needs to finish or be recovered before installing or uninstalling.';
    exit;
  end;
  BinDir := ExpandConstant('{app}\bin\64bit');
  ForceDirectories(BinDir);
  UseLock := CreateFileW(BinDir + '\bokis-twitch-chat-plugin.dll.use.lock', $C0000000, 0, 0, 4, $80, 0);
  if (UseLock = InvalidHandle) or
    not WritableBinary(BinDir + '\bokis-twitch-chat-plugin.dll') or
    not WritableBinary(BinDir + '\bokis-twitch-chat-updater.exe') or
    not WritableBinary(BinDir + '\Qt6WebSockets.dll') then
    Result := 'Close all OBS instances completely before continuing. A plugin file is loaded or not writable.';
end;

procedure InitializeWizard;
begin
  UpdateLock := InvalidHandle; UseLock := InvalidHandle;
  WizardForm.WelcomeLabel2.Caption :=
    'Close OBS completely before installing. This installs for your Windows account without administrator privileges.' + #13#10#13#10 +
    'OBS 32.2.2 or newer within OBS 32 is required. Per-user plugin discovery is registered using OBS''s supported environment settings.' + #13#10#13#10 +
    'This release is unsigned. Only continue with packages downloaded from the project release page.';
end;

function PrepareToInstall(var NeedsRestart: Boolean): string;
begin
  Result := '';
  if RegisterEnvironment and not ResolveDiscoveryDirectory then begin
    Result := 'OBS cannot safely discover this Unicode path. Choose an ASCII-only writable /DIR destination, or configure discovery yourself and use /RegisterEnv=0. See INSTALL_WINDOWS.md.';
    exit;
  end;
  if RegisterEnvironment and
    (not EnvironmentCompatible('OBS_PLUGINS_PATH', BinSearchPath) or
     not EnvironmentCompatible('OBS_PLUGINS_DATA_PATH', DataSearchPath)) then begin
    Result := 'Custom OBS plugin paths are already configured. They have been preserved. See INSTALL_WINDOWS.md for installation with existing custom paths.';
    exit;
  end;
  Result := LockInstallation;
end;

procedure RegisterOne(Name, Value: string);
var Existing, RecordFile: string;
begin
  RecordFile := ExpandConstant('{app}\environment.ini');
  if not RegQueryStringValue(HKCU, 'Environment', Name, Existing) then Existing := '';
  if Existing = '' then begin
    { Record ownership before registering; never claim a pre-existing setting. }
    if not SetIniString('Environment', Name, Value, RecordFile) then
      RaiseException('Could not record OBS plugin discovery settings.');
    if not RegWriteStringValue(HKCU, 'Environment', Name, Value) then
      RaiseException('Could not register OBS plugin discovery settings.');
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and RegisterEnvironment then begin
    RegisterOne('OBS_PLUGINS_PATH', BinSearchPath);
    RegisterOne('OBS_PLUGINS_DATA_PATH', DataSearchPath);
  end;
end;

procedure DeinitializeSetup;
begin
  ReleaseLocks;
end;

function InitializeUninstall: Boolean;
var Error: string;
begin
  UpdateLock := InvalidHandle; UseLock := InvalidHandle;
  Error := LockInstallation;
  Result := Error = '';
  if not Result then begin
    SuppressibleMsgBox(Error, mbError, MB_OK, IDOK);
    ReleaseLocks;
  end;
end;

procedure RemoveOwnedEnvironment(Name: string);
var Owned, Current: string;
begin
  Owned := GetIniString('Environment', Name, '', ExpandConstant('{app}\environment.ini'));
  if (Owned <> '') and RegQueryStringValue(HKCU, 'Environment', Name, Current) and (Current = Owned) then
    RegDeleteValue(HKCU, 'Environment', Name);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then begin
    RemoveOwnedEnvironment('OBS_PLUGINS_PATH');
    RemoveOwnedEnvironment('OBS_PLUGINS_DATA_PATH');
    DeleteFile(ExpandConstant('{app}\environment.ini'));
  end;
end;

procedure DeinitializeUninstall;
begin
  ReleaseLocks;
end;
