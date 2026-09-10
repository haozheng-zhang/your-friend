#define MyAppName "Your Friend"
#define MyAppVersion "1.1.0"
#define MyAppExeName "DesktopFly_5min.exe"

[Setup]
AppId={{F1D8FC43-3AF1-46D7-BD92-69309D567B76}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
DefaultDirName={autopf}\Your Friend
DefaultGroupName={#MyAppName}
OutputDir=..\dist
OutputBaseFilename=YourFriend_Setup_1.1.0
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#MyAppName}

[Files]
Source: "..\dist\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{userdesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加快捷方式："; Flags: unchecked

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "运行 {#MyAppName}"; Flags: nowait postinstall skipifsilent
