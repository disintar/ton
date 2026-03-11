@echo off
setlocal EnableDelayedExpansion

REM build-3pp.bat
REM Purpose: prepare third-party libs and vcpkg deps with caching.
REM Note: Most libraries are now built directly by the main CMake project.

REM Resolve repo root (current directory)
set REPO_ROOT=%cd%

REM Resolve runner temp directories provided by GitHub Actions
if "%RUNNER_TEMP%"=="" (
  echo RUNNER_TEMP is not set. Falling back to %TEMP%.
  set "RUNNER_TEMP=%TEMP%"
)

set CACHE_ROOT=%RUNNER_TEMP%\3pp_clear
set STATUS_DIR=%RUNNER_TEMP%\3pp
set STATUS_FILE=%STATUS_DIR%\3pp_status.txt

if not exist "%CACHE_ROOT%" mkdir "%CACHE_ROOT%"
if not exist "%STATUS_DIR%" mkdir "%STATUS_DIR%"

set NEED_CACHE=false

REM ------------------------------------------------------------
REM Ensure NASM is installed (might still be needed by OpenSSL built by CMake)
REM ------------------------------------------------------------
where nasm >nul 2>&1
if errorlevel 1 (
  echo NASM not found on PATH. Attempting installation...
  where choco >nul 2>&1
  if not errorlevel 1 (
    echo Installing NASM via Chocolatey...
    choco install nasm -y --no-progress
    if exist "C:\Program Files\NASM\nasm.exe" set "PATH=%PATH%;C:\Program Files\NASM"
    if exist "C:\ProgramData\chocolatey\bin\nasm.exe" set "PATH=%PATH%;C:\ProgramData\chocolatey\bin"
  )
)

REM ------------------------------------------------------------
REM Restore vcpkg from cache if present
REM ------------------------------------------------------------
set VCPKG_ROOT=C:\vcpkg
if exist "%CACHE_ROOT%\vcpkg_installed" (
  if not exist "%VCPKG_ROOT%\installed" (
    echo Restoring vcpkg installed from cache...
    if not exist "%VCPKG_ROOT%" mkdir "%VCPKG_ROOT%"
    robocopy "%CACHE_ROOT%\vcpkg_installed" "%VCPKG_ROOT%\installed" /MIR >nul
  )
)

REM ------------------------------------------------------------
REM Ensure vcpkg and required ports (librdkafka, boost)
REM ------------------------------------------------------------
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
  echo Bootstrapping vcpkg...
  git clone https://github.com/microsoft/vcpkg "%VCPKG_ROOT%"
  call "%VCPKG_ROOT%\bootstrap-vcpkg.bat"
  if errorlevel 1 (
    echo Failed to bootstrap vcpkg
    exit /b 1
  )
  set NEED_CACHE=true
)

REM Check whether boost is already installed to avoid reinstall
if not exist "%VCPKG_ROOT%\installed\x64-windows\include\boost" set NEED_CACHE=true

"%VCPKG_ROOT%\vcpkg.exe" install librdkafka:x64-windows boost:x64-windows
if errorlevel 1 (
  echo vcpkg failed to install dependencies
  exit /b 1
)

REM Expose env variables
set "prefix=%VCPKG_ROOT%\installed\x64-windows"
echo RDKAFKA_ROOT=%prefix%>> "%GITHUB_ENV%"
echo BOOST_ROOT=%prefix%>> "%GITHUB_ENV%"
echo BOOST_INCLUDEDIR=%prefix%\include>> "%GITHUB_ENV%"
echo BOOST_LIBRARYDIR=%prefix%\lib>> "%GITHUB_ENV%"

REM ------------------------------------------------------------
REM Sync build outputs back to CACHE_ROOT
REM ------------------------------------------------------------
robocopy "%VCPKG_ROOT%\installed" "%CACHE_ROOT%\vcpkg_installed" /MIR >nul

REM Write status file
> "%STATUS_FILE%" (
  if /I "%NEED_CACHE%"=="true" (
    echo NEED_CACHE=true
  ) else (
    echo NEED_CACHE=false
  )
)

echo 3pp preparation completed.
exit /b 0
