@echo off
setlocal
set "EDITION=%~1"
if "%EDITION%"=="" set "EDITION=Enterprise"

set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\%EDITION%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

call "%VCVARS%"
if errorlevel 1 exit /b %errorlevel%
call build-windows-2022.bat -t
