@echo off
rem Remove matata: URL scheme registration and the "Send to matata" shortcut.
setlocal

reg delete "HKCU\Software\Classes\matata" /f >nul 2>&1

set SENDTO=%APPDATA%\Microsoft\Windows\SendTo
if exist "%SENDTO%\matata.lnk" del "%SENDTO%\matata.lnk"

echo [uninstall-shell] matata: URL scheme and SendTo shortcut removed (if present).
exit /b 0
