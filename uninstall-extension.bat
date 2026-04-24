@echo off
rem Removes the matata native-host registration from Chrome, Edge, Chromium.
setlocal

reg delete "HKCU\Software\Google\Chrome\NativeMessagingHosts\com.matata.host"   /f >nul 2>&1
reg delete "HKCU\Software\Microsoft\Edge\NativeMessagingHosts\com.matata.host"  /f >nul 2>&1
reg delete "HKCU\Software\Chromium\NativeMessagingHosts\com.matata.host"        /f >nul 2>&1
reg delete "HKCU\Software\Mozilla\NativeMessagingHosts\com.matata.host"         /f >nul 2>&1

echo [uninstall] matata native-host entries removed (if present).
echo             matata-host.exe and the com.matata.host.json file are left
echo             on disk -- delete them manually if desired.
exit /b 0
