@echo off
rem Starts 5 DiAvarice clients with verbose logging to build-mingw64\logs\client1.log ... client5.log
setlocal
set "BUILD=%~dp0build-mingw64"
set "LOGS=%BUILD%\logs"
if not exist "%LOGS%" mkdir "%LOGS%"
for /L %%i in (1,1,5) do (
    start "client%%i" /D "%BUILD%" "%BUILD%\devilutionx.exe" --verbose --log-to-file "%LOGS%\client%%i.log"
)
endlocal
