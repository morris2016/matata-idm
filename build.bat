@echo off
rem ------------------------------------------------------------------
rem matata - build script (MSVC BuildTools 2022, x64)
rem Usage:   build.bat            -> release build
rem          build.bat debug      -> debug build
rem          build.bat clean      -> wipe build\
rem ------------------------------------------------------------------
setlocal EnableDelayedExpansion

set ROOT=%~dp0
set BUILD=%ROOT%build
set SRC=%ROOT%src
set INC=%ROOT%include

if /I "%1"=="clean" (
    if exist "%BUILD%" rmdir /S /Q "%BUILD%"
    mkdir "%BUILD%"
    echo [matata] build tree cleaned.
    exit /b 0
)

set MODE=release
if /I "%1"=="debug" set MODE=debug

set CFLAGS=/nologo /EHsc /std:c++17 /W4 /permissive- /DUNICODE /D_UNICODE /I"%INC%"
set LFLAGS=/nologo winhttp.lib ws2_32.lib shlwapi.lib advapi32.lib user32.lib bcrypt.lib crypt32.lib

rem WebView2 SDK paths (fetched by fetch-webview2.ps1).
set WV2_INC=%ROOT%third-party\webview2\include
set WV2_LIB=%ROOT%third-party\webview2\lib\x64

if "%MODE%"=="debug" (
    set CFLAGS=%CFLAGS% /Od /Zi /MDd /D_DEBUG
    set LFLAGS=%LFLAGS% /DEBUG
) else (
    set CFLAGS=%CFLAGS% /O2 /MD /DNDEBUG
)

rem ---- locate vcvars64.bat ----
set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% (
    echo [matata] cannot find vcvars64.bat at %VCVARS%
    exit /b 1
)

if not exist "%BUILD%" mkdir "%BUILD%"

rem ---- fetch WebView2 SDK on first run ----
if not exist "%WV2_INC%\WebView2.h" (
    echo [matata] WebView2 SDK not found, fetching...
    powershell -ExecutionPolicy Bypass -File "%ROOT%fetch-webview2.ps1"
    if errorlevel 1 ( echo [matata] WebView2 SDK fetch failed & exit /b 1 )
)

rem ---- build ----
call %VCVARS% >nul
if errorlevel 1 ( echo [matata] vcvars setup failed & exit /b 1 )

pushd "%BUILD%"

rem Library sources shared by CLI and GUI targets.
set LIB_SOURCES=^
 "%SRC%\url.cpp" ^
 "%SRC%\http.cpp" ^
 "%SRC%\bandwidth.cpp" ^
 "%SRC%\base64.cpp" ^
 "%SRC%\digest.cpp" ^
 "%SRC%\auth.cpp" ^
 "%SRC%\aes.cpp" ^
 "%SRC%\segments.cpp" ^
 "%SRC%\categories.cpp" ^
 "%SRC%\downloader.cpp" ^
 "%SRC%\queue.cpp" ^
 "%SRC%\hls.cpp" ^
 "%SRC%\dash.cpp" ^
 "%SRC%\video.cpp" ^
 "%SRC%\tls.cpp" ^
 "%SRC%\ftp.cpp" ^
 "%SRC%\ftp_downloader.cpp" ^
 "%SRC%\bencode.cpp" ^
 "%SRC%\torrent.cpp" ^
 "%SRC%\updater.cpp"

echo [matata] compiling matata.exe %MODE% ...
cl %CFLAGS% %LIB_SOURCES% "%SRC%\main.cpp" /Fe:matata.exe /link %LFLAGS%
set RC=%ERRORLEVEL%
if not "%RC%"=="0" goto :fail

echo [matata] compiling matata-gui.exe %MODE% (WebView2) ...
cl %CFLAGS% /I"%WV2_INC%" %LIB_SOURCES% "%SRC%\gui2_main.cpp" /Fe:matata-gui.exe /link %LFLAGS% comctl32.lib gdi32.lib ole32.lib shell32.lib version.lib "%WV2_LIB%\WebView2LoaderStatic.lib" /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup
set RC=%ERRORLEVEL%
if not "%RC%"=="0" goto :fail

echo [matata] compiling matata-host.exe %MODE% ...
cl %CFLAGS% "%ROOT%native-host\host.cpp" /Fe:matata-host.exe /link /nologo user32.lib shell32.lib ole32.lib
set RC=%ERRORLEVEL%
if not "%RC%"=="0" goto :fail

echo [matata] compiling matata_shell.dll %MODE% ...
cl %CFLAGS% /LD "%ROOT%shell-ext\matata_shell.cpp" /Fe:matata_shell.dll /link /nologo /DEF:"%ROOT%shell-ext\matata_shell.def" user32.lib shell32.lib shlwapi.lib ole32.lib advapi32.lib
set RC=%ERRORLEVEL%
if not "%RC%"=="0" goto :fail

popd
echo [matata] built: %BUILD%\matata.exe  %BUILD%\matata-gui.exe  %BUILD%\matata-host.exe  %BUILD%\matata_shell.dll
exit /b 0

:fail
popd
echo [matata] build FAILED (exit %RC%)
exit /b %RC%
