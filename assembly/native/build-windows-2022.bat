REM execute this script inside elevated (Run as Administrator) console "x64 Native Tools Command Prompt for VS 2022"

echo off

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
choco -?
IF %errorlevel% NEQ 0 (
  echo Can't install chocolatey
  exit /b %errorlevel%
)

choco feature enable -n allowEmptyChecksums

echo Installing tools...
choco install -y pkgconfiglite ninja nasm
IF %errorlevel% NEQ 0 (
  echo Can't install tools
  exit /b %errorlevel%
)
SET PATH=%PATH%;C:\Program Files\NASM

if not exist "third_libs" (
    mkdir "third_libs"
)
cd third_libs

set third_libs=%cd%
echo %third_libs%
<<<<<<< HEAD

if not exist "zlib" (
  git clone https://github.com/madler/zlib.git
  cd zlib
  git checkout v1.3.1
  cd contrib\vstudio\vc14
  msbuild zlibstat.vcxproj /p:Configuration=ReleaseWithoutAsm /p:platform=x64 -p:PlatformToolset=v143
  cd ..\..\..\..
) else (
  echo Using zlib...
)

if not exist "lz4" (
  git clone https://github.com/lz4/lz4.git
  cd lz4
  git checkout v1.9.4
  cd build\VS2022\liblz4
  msbuild liblz4.vcxproj /p:Configuration=Release /p:platform=x64 -p:PlatformToolset=v143
  cd ..\..\..\..
) else (
  echo Using lz4...
)

if not exist "libsodium" (
  git clone https://github.com/jedisct1/libsodium
  cd libsodium
  git checkout 1.0.18-RELEASE
  msbuild libsodium.vcxproj /p:Configuration=Release /p:platform=x64 -p:PlatformToolset=v143
  cd ..
) else (
  echo Using libsodium...
)

if not exist "openssl" (
  git clone https://github.com/openssl/openssl.git
  cd openssl
  git checkout openssl-3.1.4
  where perl
  perl Configure VC-WIN64A no-tests
  IF %errorlevel% NEQ 0 (
    echo Can't configure openssl
    exit /b %errorlevel%
  )
  nmake
  cd ..
) else (
  echo Using openssl...
)

if not exist "libmicrohttpd" (
  git clone https://github.com/Karlson2k/libmicrohttpd.git
  cd libmicrohttpd
  git checkout v1.0.1
  cd w32\VS2022
  msbuild libmicrohttpd.vcxproj /p:Configuration=Release-static /p:platform=x64 -p:PlatformToolset=v143
  IF %errorlevel% NEQ 0 (
    echo Can't compile libmicrohttpd
    exit /b %errorlevel%
  )
  cd ../../..
) else (
  echo Using libmicrohttpd...
)
=======
set "third_party=%ROOT_DIR%\third-party"
>>>>>>> mainnet/testnet

cd ..
echo Current dir %cd%

mkdir build
cd build
cmake -GNinja  -DCMAKE_BUILD_TYPE=Release ^
-DCCACHE_FOUND= ^
-DCMAKE_CXX_COMPILER_LAUNCHER= ^
-DPORTABLE=1 ^
<<<<<<< HEAD
-DTON_USE_PYTHON=1 ^
-DRDKAFKA_ROOT=%RDKAFKA_ROOT% ^
-DSODIUM_USE_STATIC_LIBS=1 ^
-DSODIUM_LIBRARY_RELEASE=%third_libs%\libsodium\Build\Release\x64\libsodium.lib ^
-DSODIUM_LIBRARY_DEBUG=%third_libs%\libsodium\Build\Release\x64\libsodium.lib ^
-DSODIUM_INCLUDE_DIR=%third_libs%\libsodium\src\libsodium\include ^
-DLZ4_FOUND=1 ^
-DLZ4_INCLUDE_DIRS=%third_libs%\lz4\lib ^
-DLZ4_LIBRARIES=%third_libs%\lz4\build\VS2022\liblz4\bin\x64_Release\liblz4_static.lib ^
-DMHD_FOUND=1 ^
-DMHD_LIBRARY=%third_libs%\libmicrohttpd\w32\VS2022\Output\x64\libmicrohttpd.lib ^
-DMHD_INCLUDE_DIR=%third_libs%\libmicrohttpd\src\include ^
-DZLIB_FOUND=1 ^
-DZLIB_INCLUDE_DIR=%third_libs%\zlib ^
-DZLIB_LIBRARIES=%third_libs%\zlib\contrib\vstudio\vc14\x64\ZlibStatReleaseWithoutAsm\zlibstat.lib ^
-DOPENSSL_FOUND=1 ^
-DOPENSSL_INCLUDE_DIR=%third_libs%\openssl\include ^
-DOPENSSL_CRYPTO_LIBRARY=%third_libs%\openssl\libcrypto_static.lib ^
-DOPENSSL_ROOT_DIR=%third_libs%\openssl ^
-DOPENSSL_USE_STATIC_LIBS=TRUE ^
=======
>>>>>>> mainnet/testnet
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
mkdir artifacts

REM Copy Python extension
for %%P in (build\tvm-python\*.pyd) do (
  echo copy "%%P" "artifacts\"
  copy "%%P" "artifacts\"
)
