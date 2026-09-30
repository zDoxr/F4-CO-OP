@echo off
rem Offline tests for CoSync's pre-join content sync (no Fallout 4 needed).
rem   ManifestTests: path rules, manifest parsing, hashing, plugin headers,
rem                  plugins.txt, Vortex deployment parsing.
rem   SyncTests:     the real CoSyncContentSync host + client state machines
rem                  against each other (in-memory wire, two game folders).
rem Uses the VS 2026 x64 toolchain; output goes to %TEMP%\cosync_content_tests_build.
rem Pass -v to SyncTests (edit below) for the module's own log lines.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set SRC=%~dp0..\..\DoxCoSync\Networking
set OUT=%TEMP%\cosync_content_tests_build
if not exist "%OUT%\src" mkdir "%OUT%\src"

cl /nologo /std:c++17 /EHsc /W4 /wd4100 /I"%SRC%" /I"%~dp0." /Fe"%OUT%\ManifestTests.exe" /Fo"%OUT%\\" ^
   "%~dp0ManifestTests.cpp" "%SRC%\CoSyncContentManifest.cpp" || exit /b 1
"%OUT%\ManifestTests.exe" || exit /b 1

rem The sync module is compiled from a copy so its game/network includes
rem resolve to the stubs instead of the real headers next to it.
copy /y "%SRC%\CoSyncContentSync.cpp" "%OUT%\src\" >nul
copy /y "%SRC%\CoSyncContentSync.h" "%OUT%\src\" >nul
copy /y "%SRC%\CoSyncContentManifest.h" "%OUT%\src\" >nul
copy /y "%SRC%\CoSyncContentManifest.cpp" "%OUT%\src\" >nul
cl /nologo /std:c++17 /EHsc /W4 /wd4100 /DCOSYNC_CONTENT_OFFLINE_TEST /I"%OUT%\src" /I"%~dp0stubs" ^
   /Fe"%OUT%\SyncTests.exe" /Fo"%OUT%\\" ^
   "%~dp0SyncTests.cpp" "%OUT%\src\CoSyncContentSync.cpp" "%OUT%\src\CoSyncContentManifest.cpp" || exit /b 1
"%OUT%\SyncTests.exe" %1 || exit /b 1
endlocal
