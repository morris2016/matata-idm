@echo off
rem ------------------------------------------------------------------
rem matata browser integration installer.
rem
rem Registers native messaging host for a given extension ID with
rem Chrome and Edge (HKCU). Run AFTER:
rem   1. build.bat  (produces matata.exe and matata-host.exe)
rem   2. Loading the unpacked extension/ folder in chrome://extensions
rem      and copying its 32-char extension ID.
rem
rem Usage:   install-extension.bat <EXTENSION_ID>
rem ------------------------------------------------------------------
setlocal EnableDelayedExpansion

set ROOT=%~dp0
set HOST_EXE=%ROOT%build\matata-host.exe
set HOST_JSON=%ROOT%native-host\com.matata.host.json
set HOST_JSON_FF=%ROOT%native-host\com.matata.host.firefox.json
set FF_EXT_ID=matata@matata.local

set CHROME_ID=%1
set EDGE_ID=%2
if "%CHROME_ID%"=="" (
    echo Usage: install-extension.bat CHROME_EXTENSION_ID [EDGE_EXTENSION_ID]
    echo.
    echo CHROME_EXTENSION_ID: the 32-char id shown at chrome://extensions
    echo                      ^(dev-mode load^) or assigned by the Chrome
    echo                      Web Store after publishing.
    echo EDGE_EXTENSION_ID:   optional -- the separate id assigned by
    echo                      Microsoft Edge Add-ons if you also publish
    echo                      to that store. Without this arg, Edge
    echo                      users who installed from the Chrome Web
    echo                      Store still work because they share the
    echo                      Chrome id; Edge Add-ons-store installs
    echo                      need this second id.
    echo.
    echo Firefox uses a fixed gecko id ^(matata@matata.local^) and is
    echo handled automatically -- no argument needed.
    exit /b 1
)

if not exist "%HOST_EXE%" (
    echo [install] %HOST_EXE% not found. Run build.bat first.
    exit /b 1
)

rem Escape backslashes for JSON.
set ESCAPED_PATH=%HOST_EXE:\=\\%

rem --- Chromium-family manifest (allowed_origins) ---
> "%HOST_JSON%" (
    echo {
    echo   "name": "com.matata.host",
    echo   "description": "matata download helper native host",
    echo   "path": "%ESCAPED_PATH%",
    echo   "type": "stdio",
    echo   "allowed_origins": [
    echo     "chrome-extension://%EXTID%/"
    echo   ]
    echo }
)

rem --- Firefox manifest (allowed_extensions) ---
> "%HOST_JSON_FF%" (
    echo {
    echo   "name": "com.matata.host",
    echo   "description": "matata download helper native host",
    echo   "path": "%ESCAPED_PATH%",
    echo   "type": "stdio",
    echo   "allowed_extensions": [
    echo     "%FF_EXT_ID%"
    echo   ]
    echo }
)

reg add "HKCU\Software\Google\Chrome\NativeMessagingHosts\com.matata.host"       /ve /t REG_SZ /d "%HOST_JSON%"    /f >nul
reg add "HKCU\Software\Microsoft\Edge\NativeMessagingHosts\com.matata.host"      /ve /t REG_SZ /d "%HOST_JSON%"    /f >nul
reg add "HKCU\Software\Chromium\NativeMessagingHosts\com.matata.host"            /ve /t REG_SZ /d "%HOST_JSON%"    /f >nul
reg add "HKCU\Software\Mozilla\NativeMessagingHosts\com.matata.host"             /ve /t REG_SZ /d "%HOST_JSON_FF%" /f >nul

echo.
echo [install] registered com.matata.host for Chrome, Edge, Chromium, Firefox.
echo [install] chrome manifest:  %HOST_JSON%
echo [install] firefox manifest: %HOST_JSON_FF%
echo [install] host exe:         %HOST_EXE%
echo [install] chromium ext id:  %EXTID%
echo [install] firefox ext id:   %FF_EXT_ID%
echo.
echo Restart the browser (fully quit, not just close the window) so the new
echo native messaging host is picked up, then reload the extension.
exit /b 0
