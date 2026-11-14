; TacAudio installer (Inno Setup 6).
; Built by scripts\build_installer.ps1, which stages the files into dist\stage first:
;   iscc /DAppVersion=0.4.0 installer\TacAudio.iss
;
; Per-user install (no administrator rights needed): %LOCALAPPDATA%\Programs\TacAudio.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{72790870-2B88-4F43-9930-0BE4F76860D4}
AppName=TacAudio
AppVersion={#AppVersion}
AppVerName=TacAudio {#AppVersion}
AppPublisher=TacAudio
AppPublisherURL=https://github.com/sa3oodi/TacAudio-engine
AppSupportURL=https://github.com/sa3oodi/TacAudio-engine/issues
AppUpdatesURL=https://github.com/sa3oodi/TacAudio-engine/releases
DefaultDirName={autopf}\TacAudio
DefaultGroupName=TacAudio
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir=..\dist
OutputBaseFilename=TacAudio-Setup-{#AppVersion}
SetupIconFile=..\apps\tacaudio_gui\res\tacaudio.ico
UninstallDisplayIcon={app}\TacAudio.exe
UninstallDisplayName=TacAudio
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
CloseApplications=yes
RestartApplications=no
InfoAfterFile=after_install.txt
VersionInfoVersion={#AppVersion}
VersionInfoProductName=TacAudio
VersionInfoDescription=TacAudio installer

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked
Name: "autostart"; Description: "Start TacAudio with &Windows (in the tray)"; GroupDescription: "Startup:"; Flags: unchecked

; Clean update: everything an earlier TacAudio version installed is deleted first, so no old file
; survives (the new files are copied after). Only TacAudio's own files are named here, never a
; wildcard, in case someone installed into a shared folder. Settings and profiles live in
; %APPDATA%\TacAudio and are kept.
[InstallDelete]
Type: files; Name: "{app}\TacAudio.exe"
Type: files; Name: "{app}\tacaudio-cli.exe"
Type: files; Name: "{app}\tacrender.exe"
Type: files; Name: "{app}\TacAudioApo.dll"
Type: files; Name: "{app}\tacapo-setup.exe"
Type: files; Name: "{app}\README.md"
Type: files; Name: "{app}\LICENSE.txt"
Type: files; Name: "{app}\THIRD_PARTY_NOTICES.txt"
Type: files; Name: "{app}\GAME-SETUP.txt"
Type: filesandordirs; Name: "{app}\hrtf"

[Files]
Source: "..\dist\stage\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{autoprograms}\TacAudio"; Filename: "{app}\TacAudio.exe"; Comment: "Spatial audio for competitive gaming"
Name: "{autodesktop}\TacAudio"; Filename: "{app}\TacAudio.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "TacAudio"; \
    ValueData: """{app}\TacAudio.exe"" --minimized"; Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "{app}\TacAudio.exe"; Description: "Launch TacAudio"; Flags: nowait postinstall skipifsilent
; "Update now" in the app runs this installer with /SILENT /update=1: open the new version afterwards.
Filename: "{app}\TacAudio.exe"; Flags: nowait; Check: IsAppUpdate

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/IM TacAudio.exe /F"; Flags: runhidden; RunOnceId: "StopTacAudio"

[Code]
function IsAppUpdate: Boolean;
begin
  Result := ExpandConstant('{param:update|0}') = '1';
end;

// Clean engine update: if the engine is installed in Windows audio, remove the old one completely
// and install this version's engine on the same headphones (one Windows prompt). The log goes
// where the app's "Details" button looks: %TEMP%\tacapo_setup.txt.
procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  if (CurStep = ssPostInstall) and
     RegKeyExists(HKLM64, 'SOFTWARE\Classes\CLSID\{1A057EE8-C9EB-451B-9A59-F6D6FC460F7A}') then
  begin
    WizardForm.StatusLabel.Caption := 'Updating the sound engine in Windows audio (approve the Windows prompt)...';
    if not ShellExec('runas', ExpandConstant('{app}\tacapo-setup.exe'),
                     'reinstall --log "' + GetEnv('TEMP') + '\tacapo_setup.txt"', '', SW_HIDE,
                     ewWaitUntilTerminated, Code) or (Code <> 0) then
      Log('Engine reinstall did not finish (code ' + IntToStr(Code) + '); the app offers "Update engine".');
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Code: Integer;
begin
  // Direct mode: if the engine add-on is installed in Windows audio, remove it first (restores the
  // device's original effects). Needs administrator rights, so Windows asks once.
  if (CurUninstallStep = usUninstall) and
     RegKeyExists(HKLM64, 'SOFTWARE\Classes\CLSID\{1A057EE8-C9EB-451B-9A59-F6D6FC460F7A}') then
    ShellExec('runas', ExpandConstant('{app}\tacapo-setup.exe'), 'uninstall', '', SW_HIDE, ewWaitUntilTerminated, Code);
  // The app can also create the Run entry itself (Settings > Start with Windows): always remove it.
  if CurUninstallStep = usPostUninstall then
    RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run', 'TacAudio');
end;
