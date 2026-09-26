; Inno Setup script for the TetherDesk Remote Windows installer (reach your own computers).
; Built by CI:  ISCC /DAppVersion=1.0.0 /DStageDir=<folder with the files> tetherdesk-remote.iss

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif

[Setup]
AppId={{3E7B2D51-94A6-4F0C-B8D2-51C7A6E0F913}
AppName=TetherDesk Remote
AppVersion={#AppVersion}
AppPublisher=TetherDesk
DefaultDirName={autopf}\TetherDesk Remote
DefaultGroupName=TetherDesk Remote
UninstallDisplayIcon={app}\TetherDesk-Remote.exe
OutputBaseFilename=TetherDesk-Remote-Setup-{#AppVersion}
SetupIconFile=tetherdesk.ico
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequiredOverridesAllowed=dialog

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\TetherDesk Remote"; Filename: "{app}\TetherDesk-Remote.exe"
Name: "{group}\Uninstall TetherDesk Remote"; Filename: "{uninstallexe}"
Name: "{autodesktop}\TetherDesk Remote"; Filename: "{app}\TetherDesk-Remote.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\TetherDesk-Remote.exe"; Description: "Launch TetherDesk Remote"; Flags: nowait postinstall skipifsilent
