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
#define MyAppVersion   "0.9.7"
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
Source: "..\third-party\yt-dlp.exe";      DestDir: "{app}"; Flags: ignoreversion
Source: "..\third-party\ffmpeg.exe";      DestDir: "{app}"; Flags: ignoreversion
Source: "..\ui\index.html";               DestDir: "{app}\ui"; Flags: ignoreversion
Source: "..\ui\app.js";                   DestDir: "{app}\ui"; Flags: ignoreversion
Source: "..\ui\styles.css";               DestDir: "{app}\ui"; Flags: ignoreversion
Source: "..\extension\manifest.json";     DestDir: "{app}\extension"; Flags: ignoreversion
Source: "..\extension\background.js";     DestDir: "{app}\extension"; Flags: ignoreversion
Source: "..\extension\popup.html";        DestDir: "{app}\extension"; Flags: ignoreversion
Source: "..\extension\popup.js";          DestDir: "{app}\extension"; Flags: ignoreversion
Source: "..\extension\confirm.html";      DestDir: "{app}\extension"; Flags: ignoreversion
Source: "..\extension\confirm.js";        DestDir: "{app}\extension"; Flags: ignoreversion
Source: "..\README.md";                   DestDir: "{app}"; Flags: ignoreversion
; WebView2 runtime bootstrapper — only extracted at install time if the
; runtime is missing. Adds ~1.7 MB to the installer payload.
Source: "..\third-party\MicrosoftEdgeWebview2Setup.exe"; Flags: dontcopy noencryption

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

[Code]
{ Returns true if the WebView2 evergreen runtime is already installed. The
  bootstrapper writes its version to one of these registry keys; presence of
  any non-empty 'pv' value means an installed runtime. }
function IsWebView2RuntimeInstalled(): Boolean;
var
  pv: String;
  guid: String;
begin
  guid := '{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}';
  Result := False;
  if RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\' + guid, 'pv', pv) and (pv <> '') and (pv <> '0.0.0.0') then begin
    Result := True; exit;
  end;
  if RegQueryStringValue(HKLM, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\' + guid, 'pv', pv) and (pv <> '') and (pv <> '0.0.0.0') then begin
    Result := True; exit;
  end;
  if RegQueryStringValue(HKCU, 'Software\Microsoft\EdgeUpdate\Clients\' + guid, 'pv', pv) and (pv <> '') and (pv <> '0.0.0.0') then begin
    Result := True; exit;
  end;
end;

{ Extracts and runs the WebView2 evergreen bootstrapper, falling through a
  cascade of strategies until the runtime is verified installed. Each call
  is a no-op if the runtime is already present, so this is safe even when
  earlier attempts partially succeeded.

  Cascade:
    1. silent per-machine     (/silent /install)
    2. silent per-user MSI    (/silent /install /msi)
    3. visible installer      (/install)            -- user sees Microsoft's UI
    4. as-shipped no flags    (bootstrapper picks)  -- last resort
  We NEVER block or fail the matata install; the in-app error UI handles the
  remaining edge case where every strategy somehow failed. }
function TryRunBootstrapper(const tmp, flags: String; show: Integer): Boolean;
var
  rc: Integer;
begin
  Result := False;
  Exec(tmp, flags, '', show, ewWaitUntilTerminated, rc);
  Result := IsWebView2RuntimeInstalled();
end;

procedure InstallWebView2Runtime();
var
  tmp: String;
begin
  ExtractTemporaryFile('MicrosoftEdgeWebview2Setup.exe');
  tmp := ExpandConstant('{tmp}\MicrosoftEdgeWebview2Setup.exe');
  WizardForm.StatusLabel.Caption := 'Installing Microsoft Edge WebView2 Runtime...';
  WizardForm.StatusLabel.Visible := True;

  if TryRunBootstrapper(tmp, '/silent /install',        SW_HIDE) then exit;
  if TryRunBootstrapper(tmp, '/silent /install /msi',   SW_HIDE) then exit;
  if TryRunBootstrapper(tmp, '/install',                SW_SHOWNORMAL) then exit;
  TryRunBootstrapper(tmp, '', SW_SHOWNORMAL);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    if not IsWebView2RuntimeInstalled() then
      InstallWebView2Runtime();
  end;
end;
