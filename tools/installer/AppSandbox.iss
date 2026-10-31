; AppSandbox.iss - the App Sandbox installer (Inno Setup 6).
;
; Build with build-installer.ps1, which passes the defines below. The version comes
; from Directory.Build.props through that script; there is no version in this file.
;
; There are no host drivers or services to install. Everything under drivers\ is
; copied into guests when a VM is provisioned, and the app needs no service. The
; host's only feature dependency, Virtual Machine Platform, is Windows' own: the
; app offers to enable it on first run, and uninstalling leaves it on because other
; software (WSL2) relies on it.

#ifndef AppVersion
  #error Pass /DAppVersion (use build-installer.ps1)
#endif
#ifndef StageDir
  #error Pass /DStageDir (use build-installer.ps1)
#endif
#ifndef OutputDir
  #error Pass /DOutputDir (use build-installer.ps1)
#endif
#ifndef Arch
  #define Arch "x64"
#endif

; The same mutex AppSandbox.exe and the headless daemon hold for their lifetime.
#define AppMutexName "Global\AppSandboxCoreHost"

[Setup]
; Must match ASB_INSTALLER_APP_ID in src\app_win\update_config.h. Never change it.
AppId={{7D3F2A64-9B1E-4C58-A0D7-3E6C1F82B954}
AppName=App Sandbox
AppVersion={#AppVersion}
AppVerName=App Sandbox {#AppVersion}
AppPublisher=App Sandbox
AppPublisherURL=https://github.com/lunaboppin/appsandbox
AppSupportURL=https://github.com/lunaboppin/appsandbox/issues
AppUpdatesURL=https://github.com/lunaboppin/appsandbox/releases
VersionInfoVersion={#AppVersion}
VersionInfoProductName=App Sandbox
VersionInfoDescription=App Sandbox Setup
DefaultDirName={autopf}\App Sandbox
DefaultGroupName=App Sandbox
DisableProgramGroupPage=yes
UsePreviousAppDir=yes
#ifdef TestPerUser
; For tools\installer tests only: lets the install, layout and uninstall be exercised
; without a UAC prompt. Never published (build-installer.ps1 refuses to sign it).
PrivilegesRequired=lowest
#else
PrivilegesRequired=admin
#endif
#if Arch == "arm64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#else
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
#endif
MinVersion=10.0.22000
OutputDir={#OutputDir}
OutputBaseFilename=AppSandbox-{#AppVersion}-win-{#Arch}-setup
SetupIconFile=..\..\src\app_win\appsandbox.ico
UninstallDisplayIcon={app}\AppSandbox.exe
UninstallDisplayName=App Sandbox
LicenseFile=..\..\LICENSE
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
SetupLogging=yes
; The script below decides what happens when the app is running: it must never be
; closed behind the user's back, because closing it terminates every running VM.
CloseApplications=no
RestartApplications=no
SetupMutex=AppSandboxSetup,Global\AppSandboxSetup
#ifdef SignTool
SignTool=asb
SignedUninstaller=yes
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[InstallDelete]
; Replaced whole on every upgrade, so a file a new version dropped doesn't linger.
; Nothing writes to these at runtime; the runtime caches below stay across upgrades.
Type: filesandordirs; Name: "{app}\web"
Type: filesandordirs; Name: "{app}\resources"
Type: filesandordirs; Name: "{app}\drivers"
Type: filesandordirs; Name: "{app}\headless-api"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\THIRD-PARTY-NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
; The working folder matters: the app writes appsandbox.log to its current directory.
Name: "{autoprograms}\App Sandbox"; Filename: "{app}\AppSandbox.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\App Sandbox"; Filename: "{app}\AppSandbox.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
; Explorer's right-click entry, for every file type. On Windows 11 it sits under "Show more
; options": the short menu only takes packaged extensions. The launcher runs unelevated, so
; the entry raises no UAC prompt while the app is open. HKA follows the install mode, and an
; upgrade rewrites the same key with the current folder.
Root: HKA; Subkey: "Software\Classes\*\shell\AppSandbox.RunInSandbox"; ValueType: string; ValueName: ""; ValueData: "Run in Sandbox"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\*\shell\AppSandbox.RunInSandbox"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\AppSandbox.exe,0"
Root: HKA; Subkey: "Software\Classes\*\shell\AppSandbox.RunInSandbox"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Document"
Root: HKA; Subkey: "Software\Classes\*\shell\AppSandbox.RunInSandbox\command"; ValueType: string; ValueName: ""; ValueData: """{app}\appsandbox-run.exe"" ""%1"""

[Run]
; An update reopens the app from DeinitializeSetup instead, so that it comes back
; even if the update failed.
Filename: "{app}\AppSandbox.exe"; WorkingDir: "{app}"; Description: "Start App Sandbox"; Flags: nowait postinstall skipifsilent runascurrentuser

[UninstallDelete]
; What the app downloads or writes beside itself at runtime. Named one by one, not
; the whole folder: the user may have installed into a folder holding other files.
Type: filesandordirs; Name: "{app}\d3dlayers"
Type: files; Name: "{app}\OpenSSH-*.msi"
Type: files; Name: "{app}\appsandbox.log"
Type: files; Name: "{app}\appsandbox.log.prev"
Type: dirifempty; Name: "{app}"

[Code]
const
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{7D3F2A64-9B1E-4C58-A0D7-3E6C1F82B954}_is1';
  UpdateWaitSeconds = 120;

// Started by the app's updater (updater_launch_installer).
function IsUpdateRun: Boolean;
begin
  Result := ExpandConstant('{param:ASBUPDATE|0}') = '1';
end;

function AppIsRunning: Boolean;
begin
  Result := CheckForMutexes('{#AppMutexName}');
end;

function InitializeSetup: Boolean;
var
  Waited: Integer;
begin
  Result := True;
  if IsUpdateRun then begin
    // The app closes itself right after starting this, having checked no VM is
    // running. Give it time; if it never lets go, leave everything as it was.
    Waited := 0;
    while AppIsRunning and (Waited < UpdateWaitSeconds) do begin
      Sleep(1000);
      Waited := Waited + 1;
    end;
    if AppIsRunning then begin
      Log('App Sandbox did not close in time; update abandoned.');
      Result := False;
    end;
    exit;
  end;

  while AppIsRunning do begin
    if WizardSilent then begin
      Log('App Sandbox is running; a silent install will not close it.');
      Result := False;
      exit;
    end;
    if MsgBox('App Sandbox is running.'#13#10#13#10 +
              'Closing it stops every running VM, so shut your VMs down first, ' +
              'then exit App Sandbox from its tray icon and click Retry.',
              mbInformation, MB_RETRYCANCEL) <> IDRETRY then begin
      Result := False;
      exit;
    end;
  end;
end;

// Runs however Setup ends, so a failed update still gives the user their app back.
procedure DeinitializeSetup;
var
  Location: String;
  ResultCode: Integer;
begin
  if not IsUpdateRun then exit;
  if RegQueryStringValue(HKLM64, UninstallKey, 'InstallLocation', Location) then
    ShellExec('', AddBackslash(Location) + 'AppSandbox.exe', '', Location, SW_SHOWNORMAL, ewNoWait, ResultCode);
end;

function InitializeUninstall: Boolean;
begin
  Result := True;
  while AppIsRunning do begin
    if UninstallSilent then begin
      Log('App Sandbox is running; a silent uninstall will not close it.');
      Result := False;
      exit;
    end;
    if MsgBox('App Sandbox is running.'#13#10#13#10 +
              'Closing it stops every running VM, so shut your VMs down first, ' +
              'then exit App Sandbox from its tray icon and click Retry.',
              mbInformation, MB_RETRYCANCEL) <> IDRETRY then begin
      Result := False;
      exit;
    end;
  end;
end;

// The VMMS certificate the app created so VMs can use integration services
// (src\backend_win\vmms_cert.c), recognised by its subject. Hyper-V's own certificate isn't touched.
procedure RemoveVmmsCertificate;
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
       '-NoProfile -NonInteractive -Command "Get-ChildItem Cert:\LocalMachine\My | ' +
       'Where-Object { $_.Subject -eq (''CN=AppSandbox-'' + $env:COMPUTERNAME) } | Remove-Item -DeleteKey"',
       '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep <> usPostUninstall then exit;
  DataDir := ExpandConstant('{commonappdata}\AppSandbox');
  // VMs are the user's data, so they stay unless the user says otherwise, and a
  // silent uninstall (nobody to ask) always keeps them.
  if UninstallSilent or not DirExists(DataDir) then exit;
  if MsgBox('Also delete your App Sandbox VMs and settings?'#13#10#13#10 +
            'This permanently deletes ' + DataDir + ', including VM disks, checkpoints, ' +
            'SSH keys and saved VPN configs. VMs you placed in other folders are not removed.'#13#10#13#10 +
            'Choose No to keep them for a later reinstall.',
            mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then begin
    DelTree(DataDir, True, True, True);
    RemoveVmmsCertificate;
  end;
end;
