; NetConditioner 安装包脚本（Inno Setup 6.7+）
; 由 installer\build-installer.ps1 调用 ISCC.exe 编译。

#define AppId "{{7E1F4A9C-3D2B-4B8E-9F6A-1C5D2E8B7A34}"
#define AppName "NetConditioner 弱网模拟器"
#define AppShortName "NetConditioner"
#define AppPublisher "NetConditioner"
#define AppExeName "NetConditioner.exe"
#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef AppVersionNumeric
  #define AppVersionNumeric "1.0.0.0"
#endif

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\{#AppShortName}
DefaultGroupName={#AppShortName}
DisableProgramGroupPage=yes
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#AppExeName}
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0
PrivilegesRequired=admin
OutputDir=..\dist
OutputBaseFilename={#AppShortName}-{#AppVersion}-win-x64-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
SetupLogging=yes
AppMutex=Global\NetConditioner.SingleInstance
VersionInfoVersion={#AppVersionNumeric}
VersionInfoCompany={#AppPublisher}
VersionInfoDescription={#AppName} 安装程序
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#AppVersion}

[Languages]
Name: "chinesesimplified"; MessagesFile: "ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\publish\win-x64-selfcontained\*"; DestDir: "{app}"; Excludes: "*.pdb"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion isreadme

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Comment: "系统级上行 / 下行弱网模拟器"
Name: "{group}\卸载 {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
