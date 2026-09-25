; Inno Setup script for the TetherDesk Windows installer.
; Built by CI:  ISCC /DAppVersion=1.0.0 /DStageDir=<folder with the files> tetherdesk.iss

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif

[Setup]
AppId={{8C1F4A7E-2B61-4C3B-9E0B-6D2B7F1A9C44}
AppName=TetherDesk
AppVersion={#AppVersion}
AppPublisher=TetherDesk
DefaultDirName={autopf}\TetherDesk
DefaultGroupName=TetherDesk
UninstallDisplayIcon={app}\TetherDesk.exe
OutputBaseFilename=TetherDesk-Setup-{#AppVersion}
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
Name: "{group}\TetherDesk"; Filename: "{app}\TetherDesk.exe"
Name: "{group}\Uninstall TetherDesk"; Filename: "{uninstallexe}"
Name: "{autodesktop}\TetherDesk"; Filename: "{app}\TetherDesk.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\TetherDesk.exe"; Description: "Launch TetherDesk"; Flags: nowait postinstall skipifsilent
