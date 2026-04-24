; matata installer (Inno Setup 6+)
; Compile with ISCC.exe (from https://jrsoftware.org/isdl.php):
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\matata.iss
;
; Produces build\installer\matata-<version>-setup.exe, a single-file
; installer that:
;   - copies all four build artifacts into %ProgramFiles%\matata
;   - copies the browser-extension source tree
;   - registers the matata: URL scheme (HKCU)
;   - drops a Send to \ matata shortcut
;   - registers matata_shell.dll with regsvr32 (for the Explorer
;     context-menu handler)
;   - optionally adds a Start-Menu shortcut to matata-gui.exe

#define MyAppName      "matata"
#define MyAppVersion   "0.7.1"
#define MyAppPublisher "matata"
#define MyAppURL       "https://matata.example/"
#define MyAppExeName   "matata-gui.exe"

[Setup]
AppId={{B0C5A1F2-3E77-4F4D-A8D9-5B2D9F2E8C10}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
DefaultDirName={autopf}\matata
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
DisableProgramGroupPage=yes
OutputDir=..\build\installer
OutputBaseFilename=matata-{#MyAppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#MyAppExeName}
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon";       Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"
Name: "registershell";     Description: "Add 'Download with matata' to the Explorer right-click menu"; GroupDescription: "Integration:"
Name: "registerscheme";    Description: "Register the matata: URL scheme"; GroupDescription: "Integration:"

[Files]
Source: "..\build\matata.exe";            DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\matata-gui.exe";        DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\matata-host.exe";       DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\matata_shell.dll";      DestDir: "{app}"; Flags: ignoreversion regserver; Tasks: registershell
Source: "..\extension\manifest.json";     DestDir: "{app}\extension"; Flags: ignoreversion
Source: "..\extension\background.js";     DestDir: "{app}\extension"; Flags: ignoreversion
Source: "..\extension\popup.html";        DestDir: "{app}\extension"; Flags: ignoreversion
Source: "..\extension\popup.js";          DestDir: "{app}\extension"; Flags: ignoreversion
Source: "..\README.md";                   DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}";             Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{userdesktop}\{#MyAppName}";       Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon
Name: "{usersendto}\matata";              Filename: "{app}\{#MyAppExeName}"; Comment: "Send to matata"

[Registry]
; matata: URL scheme
Root: HKCU; Subkey: "Software\Classes\matata"; ValueType: string; ValueName: ""; ValueData: "URL:matata Protocol"; Flags: uninsdeletekey; Tasks: registerscheme
Root: HKCU; Subkey: "Software\Classes\matata"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""; Tasks: registerscheme
Root: HKCU; Subkey: "Software\Classes\matata\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"; Tasks: registerscheme
Root: HKCU; Subkey: "Software\Classes\matata\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: registerscheme

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent
