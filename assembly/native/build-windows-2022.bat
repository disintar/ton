@echo off
setlocal EnableDelayedExpansion

REM execute this script inside elevated (Run as Administrator) console "x64 Native Tools Command Prompt for VS 2022"

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%.") do set "SCRIPT_DIR=%%~fI"
set "ROOT_DIR=%SCRIPT_DIR%"
if not exist "%ROOT_DIR%\third-party" (
  for %%I in ("%SCRIPT_DIR%\..\..") do set "ROOT_DIR=%%~fI"
)

echo Using repo root: %ROOT_DIR%
cd /d "%ROOT_DIR%"

echo Installing chocolatey windows package manager...
@"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -InputFormat None -ExecutionPolicy Bypass -Command "iex ((New-Object System.Net.WebClient).DownloadString('https://chocolatey.org/install.ps1'))" && SET "PATH=%PATH%;%ALLUSERSPROFILE%\chocolatey\bin"
where choco >nul 2>&1
IF %errorlevel% NEQ 0 (
  echo Can't install/find chocolatey
  exit /b %errorlevel%
)

choco feature enable -n allowEmptyChecksums

echo Installing tools...
choco install -y pkgconfiglite ninja nasm
IF %errorlevel% NEQ 0 (
  echo Can't install tools
  exit /b %errorlevel%
)
SET "PATH=%PATH%;C:\Program Files\NASM"

if not exist "third_libs" (
    mkdir "third_libs"
)
set "third_libs=%ROOT_DIR%\third_libs"
set "third_party=%ROOT_DIR%\third-party"

# Building dependencies from third-party submodules...
REM All dependencies are now built over existing cmakefiles in the main project build.

cd /d "%ROOT_DIR%"
if not exist build (
  mkdir build
)
cd build

cmake -GNinja  -DCMAKE_BUILD_TYPE=Release ^
-DPORTABLE=1 ^
-DTON_USE_PYTHON=1 ^
-DRDKAFKA_ROOT=%RDKAFKA_ROOT% ^
-DCMAKE_CXX_FLAGS="/DTD_WINDOWS=1 /EHsc /bigobj" ..

IF %errorlevel% NEQ 0 (
  echo Can't configure TON
  exit /b %errorlevel%
)

ninja python_ton
IF %errorlevel% NEQ 0 (
  echo Build TON failed
  exit /b %errorlevel%
)

echo Strip and copy artifacts
cd ..
if not exist artifacts (
  mkdir artifacts
)

REM Copy Python extension
for %%P in (build\tvm-python\*.pyd) do (
  echo copy "%%P" "artifacts\"
  copy "%%P" "artifacts\"
)

echo ✅ Build and artifact collection completed successfully.
