@echo off
setlocal EnableDelayedExpansion

REM build-3pp.bat
REM Purpose: prepare third-party libs and vcpkg deps with caching, similar to universal workflow.
REM Notes: This script expects to run from the repository root on GitHub Actions Windows runners.

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
REM Ensure NASM is installed for OpenSSL build
REM ------------------------------------------------------------
where nasm >nul 2>&1
if errorlevel 1 (
  echo Installing NASM via Chocolatey...
  choco install -y nasm
  if errorlevel 1 (
    echo Failed to install NASM
    exit /b 1
  )
  set "PATH=%PATH%;C:\Program Files\NASM"
)
where nasm
if errorlevel 1 (
  echo NASM is still not available on PATH
  exit /b 1
)

REM ------------------------------------------------------------
REM Restore from cache if present
REM ------------------------------------------------------------
if exist "%CACHE_ROOT%\third_libs" (
  if not exist "%REPO_ROOT%\third_libs" (
    echo Restoring third_libs from cache...
    robocopy "%CACHE_ROOT%\third_libs" "%REPO_ROOT%\third_libs" /MIR >nul
  ) else (
    echo third_libs directory already exists. Skipping restore.
  )
)

REM vcpkg restore into C:\vcpkg if cached
set VCPKG_ROOT=C:\vcpkg
if exist "%CACHE_ROOT%\vcpkg_installed" (
  if not exist "%VCPKG_ROOT%\installed" (
    echo Restoring vcpkg installed from cache...
    if not exist "%VCPKG_ROOT%" mkdir "%VCPKG_ROOT%"
    robocopy "%CACHE_ROOT%\vcpkg_installed" "%VCPKG_ROOT%\installed" /MIR >nul
  )
)
if exist "%CACHE_ROOT%\vcpkg_downloads" (
  if not exist "%VCPKG_ROOT%\downloads" (
    echo Restoring vcpkg downloads from cache...
    if not exist "%VCPKG_ROOT%" mkdir "%VCPKG_ROOT%"
    robocopy "%CACHE_ROOT%\vcpkg_downloads" "%VCPKG_ROOT%\downloads" /MIR >nul
  )
)

REM ------------------------------------------------------------
REM Ensure vcpkg and required ports
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

REM Ensure vcpkg cache directories exist
if not exist "%VCPKG_ROOT%\installed" mkdir "%VCPKG_ROOT%\installed"
if not exist "%VCPKG_ROOT%\downloads" mkdir "%VCPKG_ROOT%\downloads"

REM Check whether boost is already installed to avoid reinstall when restored from cache
if not exist "%VCPKG_ROOT%\installed\x64-windows\include\boost" set NEED_CACHE=true

"%VCPKG_ROOT%\vcpkg.exe" install librdkafka:x64-windows
if errorlevel 1 (
  echo vcpkg failed to install librdkafka
  exit /b 1
)
"%VCPKG_ROOT%\vcpkg.exe" install boost:x64-windows
if errorlevel 1 (
  echo vcpkg failed to install boost
  exit /b 1
)

REM Expose env variables for subsequent workflow steps
set "prefix=%VCPKG_ROOT%\installed\x64-windows"
echo RDKAFKA_ROOT=%prefix%>> "%GITHUB_ENV%"
echo BOOST_ROOT=%prefix%>> "%GITHUB_ENV%"
echo BOOST_INCLUDEDIR=%prefix%\include>> "%GITHUB_ENV%"
echo BOOST_LIBRARYDIR=%prefix%\lib>> "%GITHUB_ENV%"

REM ------------------------------------------------------------
REM Build local third-party libraries only if missing
REM ------------------------------------------------------------
if not exist "%REPO_ROOT%\third_libs" mkdir "%REPO_ROOT%\third_libs"
pushd "%REPO_ROOT%\third_libs"

REM zlib
if not exist "zlib" (
  set NEED_CACHE=true
  git clone https://github.com/madler/zlib.git
  pushd zlib
  git checkout v1.3.1
  pushd contrib\vstudio\vc14
  msbuild zlibstat.vcxproj /p:Configuration=ReleaseWithoutAsm /p:platform=x64 -p:PlatformToolset=v143
  if errorlevel 1 exit /b 1
  popd & popd
) else (
  echo Using cached zlib
)

REM lz4
if not exist "lz4" (
  set NEED_CACHE=true
  git clone https://github.com/lz4/lz4.git
  pushd lz4
  git checkout v1.9.4
  pushd build\VS2022\liblz4
  msbuild liblz4.vcxproj /p:Configuration=Release /p:platform=x64 -p:PlatformToolset=v143
  if errorlevel 1 exit /b 1
  popd & popd
) else (
  echo Using cached lz4
)

REM libsodium
if not exist "libsodium" (
  set NEED_CACHE=true
  git clone https://github.com/jedisct1/libsodium
  pushd libsodium
  git checkout 1.0.18-RELEASE
  msbuild libsodium.vcxproj /p:Configuration=Release /p:platform=x64 -p:PlatformToolset=v143
  if errorlevel 1 exit /b 1
  popd
) else (
  echo Using cached libsodium
)

REM openssl
if not exist "openssl" (
  set NEED_CACHE=true
  git clone https://github.com/openssl/openssl.git
  pushd openssl
  git checkout openssl-3.1.4
  where perl
  if errorlevel 1 (
    echo Perl not found for OpenSSL build
    exit /b 1
  )
  perl Configure VC-WIN64A
  if errorlevel 1 exit /b 1
  nmake
  if errorlevel 1 exit /b 1
  popd
) else (
  echo Using cached openssl
)

REM libmicrohttpd
if not exist "libmicrohttpd" (
  set NEED_CACHE=true
  git clone https://github.com/Karlson2k/libmicrohttpd.git
  pushd libmicrohttpd
  git checkout v1.0.1
  pushd w32\VS2022
  msbuild libmicrohttpd.vcxproj /p:Configuration=Release-static /p:platform=x64 -p:PlatformToolset=v143
  if errorlevel 1 (
    echo Can't compile libmicrohttpd
    exit /b 1
  )
  popd & popd
) else (
  echo Using cached libmicrohttpd
)

popd

REM ------------------------------------------------------------
REM Prepare cache outputs (mirror to %CACHE_ROOT%)
REM ------------------------------------------------------------
robocopy "%REPO_ROOT%\third_libs" "%CACHE_ROOT%\third_libs" /MIR >nul
robocopy "%VCPKG_ROOT%\installed" "%CACHE_ROOT%\vcpkg_installed" /MIR >nul
robocopy "%VCPKG_ROOT%\downloads" "%CACHE_ROOT%\vcpkg_downloads" /MIR >nul

REM Write status file indicating whether we had to build or bootstrap
> "%STATUS_FILE%" (
  if /I "%NEED_CACHE%"=="true" (
    echo NEED_CACHE=true
  ) else (
    echo NEED_CACHE=false
  )
)

echo 3pp preparation completed. NEED_CACHE=%NEED_CACHE%
exit /b 0
