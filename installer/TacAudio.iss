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

[Files]
Source: "..\dist\stage\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{autoprograms}\TacAudio"; Filename: "{app}\TacAudio.exe"; Comment: "Spatial audio for competitive gaming"
Name: "{autodesktop}\TacAudio"; Filename: "{app}\TacAudio.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "TacAudio"; \
    ValueData: """{app}\TacAudio.exe"" --minimized"; Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "https://vb-audio.com/Cable/"; Description: "Open the VB-CABLE download page (free, required for games)"; \
    Flags: postinstall shellexec skipifsilent unchecked
Filename: "{app}\TacAudio.exe"; Description: "Launch TacAudio"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/IM TacAudio.exe /F"; Flags: runhidden; RunOnceId: "StopTacAudio"

[Code]
// The app can also create the Run entry itself (Settings > Start with Windows): always remove it.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run', 'TacAudio');
end;
