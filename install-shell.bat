@echo off
rem ------------------------------------------------------------------
rem matata shell integration installer.
rem
rem - Registers the "matata:" URL scheme (per RFC 3986, stored under
rem   HKCU so no admin needed). Any click on a matata://... link in a
rem   browser, dialog, or shortcut launches matata-gui.exe with the
rem   URL as the command line.
rem
rem - Drops a "Send to -> matata" shortcut in %APPDATA%\...\SendTo\
rem   so right-click -> Send to -> matata queues the selected file
rem   via matata-gui.exe's command-line handler.
rem
rem Usage:   install-shell.bat
rem ------------------------------------------------------------------
setlocal

set ROOT=%~dp0
set GUI=%ROOT%build\matata-gui.exe

if not exist "%GUI%" (
    echo [install-shell] %GUI% not found. Run build.bat first.
    exit /b 1
)

rem ---- URL-scheme "matata:" ----------------------------------------
reg add "HKCU\Software\Classes\matata"              /ve   /t REG_SZ /d "URL:matata Protocol"    /f >nul
reg add "HKCU\Software\Classes\matata"              /v "URL Protocol" /t REG_SZ /d ""           /f >nul
reg add "HKCU\Software\Classes\matata\shell"        /ve   /t REG_SZ /d "open"                   /f >nul
reg add "HKCU\Software\Classes\matata\shell\open"   /ve   /t REG_SZ /d ""                       /f >nul
reg add "HKCU\Software\Classes\matata\shell\open\command"  /ve /t REG_SZ /d "\"%GUI%\" \"%%1\"" /f >nul
reg add "HKCU\Software\Classes\matata\DefaultIcon"  /ve   /t REG_SZ /d "\"%GUI%\",0"            /f >nul

rem ---- SendTo shortcut --------------------------------------------
set SENDTO=%APPDATA%\Microsoft\Windows\SendTo
if not exist "%SENDTO%" (
    echo [install-shell] SendTo folder not found at %SENDTO%
    echo [install-shell] skipping SendTo shortcut.
) else (
    set SCRIPT=%TEMP%\matata-sendto.vbs
    > "%TEMP%\matata-sendto.vbs" (
        echo Set sh = CreateObject^("WScript.Shell"^)
        echo Set lnk = sh.CreateShortcut^("%SENDTO%\matata.lnk"^)
        echo lnk.TargetPath = "%GUI%"
        echo lnk.IconLocation = "%GUI%,0"
        echo lnk.Description = "matata download manager"
        echo lnk.Save
    )
    cscript //nologo "%TEMP%\matata-sendto.vbs" >nul
    del "%TEMP%\matata-sendto.vbs" >nul 2>&1
    echo [install-shell] SendTo: %SENDTO%\matata.lnk
)

echo [install-shell] matata:// scheme registered.
echo [install-shell]   try in any browser: matata://https://proof.ovh.net/files/1Mb.dat
exit /b 0
