# export NIX_PATH=nixpkgs=https://github.com/nixOS/nixpkgs/archive/23.05.tar.gz

{
  pkgs ? import <nixpkgs> { inherit system; }
, lib ? pkgs.lib
, stdenv ? pkgs.stdenv
, system ? builtins.currentSystem
, src ? ./.
}:

let
  pkgs = import <nixpkgs> {
    inherit system;
  };

  stdenv = pkgs.overrideCC pkgs.stdenv pkgs.clang_16;

  microhttpdmy = (import ./microhttpd.nix) { inherit pkgs; };
  staticLibs = import ./static-libs.nix { inherit pkgs; };

in
stdenv.mkDerivation {
  pname = "ton";
  version = "dev-bin";
  src = ./.;

  nativeBuildInputs = with pkgs; [
    clang_16 cmake ninja git pkg-config
  ];

  buildInputs = with pkgs; [
    pkgsStatic.openssl
    microhttpdmy
    pkgsStatic.zlib
    pkgsStatic.libsodium.dev
    pkgsStatic.secp256k1
    glibc.static
    staticLibs.staticBoost
    staticLibs.staticLibrdkafka
    staticLibs.staticLZ4
    staticLibs.staticLibiconv
  ];

  makeStatic = true;
  doCheck = true;

  postPatch = ''
  sed -i '/CMAKE_FLAGS.*-DINCLUDE_DIRECTORIES=.*")$/a \
    message(STATUS "RdKafka version test failed, gathering CMakeError.log…") \
    set(_rdk_err_log \"\") \
    foreach(_p \
      \"''${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/CMakeTmp/CMakeError.log\" \
      \"''${CMAKE_CURRENT_BINARY_DIR}/../CMakeFiles/CMakeTmp/CMakeError.log\" \
      \"''${CMAKE_BINARY_DIR}/CMakeFiles/CMakeTmp/CMakeError.log\" \
      \"''${CMAKE_BINARY_DIR}/CMakeFiles/CMakeError.log\" \
      \"''${CMAKE_BINARY_DIR}/CMakeError.log\" \
    ) \
      if(EXISTS \"''${_p}\") \
        set(_rdk_err_log \"''${_p}\") \
        break() \
      endif() \
    endforeach() \
    if(_rdk_err_log) \
      file(READ \"''${_rdk_err_log}\" _rdk_err) \
    else() \
      set(_rdk_err \"ERROR: could not locate CMakeError.log to show actual compile errors\") \
    endif() \
    message(FATAL_ERROR \"Failed to find valid rdkafka version.\n\nCompiler errors (from ''${_rdk_err_log}):\n\n''${_rdk_err}\")' \
    third-party/cppkafka/cmake/FindRdKafka.cmake
'';

  cmakeFlags = [
    "-DTON_USE_ABSEIL=OFF"
    "-DNIX=ON"
    "-DBUILD_SHARED_LIBS=OFF"
    "-DCMAKE_LINK_SEARCH_START_STATIC=ON"
    "-DCMAKE_LINK_SEARCH_END_STATIC=ON"
    "-DMHD_FOUND=1"
    "-DMHD_INCLUDE_DIR=${microhttpdmy}/usr/local/include"
    "-DMHD_LIBRARY=${microhttpdmy}/usr/local/lib/libmicrohttpd.a"
    "-DCMAKE_C_COMPILER=clang"
    "-DCMAKE_CXX_COMPILER=clang++"
    "-DCMAKE_CXX_STANDARD=20"
    "-DCMAKE_CXX_FLAGS=-std=c++20 -Wno-deprecated-declarations -Wno-unused-but-set-variable -w"
    "-DCMAKE_C_FLAGS=-w"
    "-DCMAKE_CTEST_ARGUMENTS=--timeout;1800"
  ];

  preConfigure = ''
    echo ">>> linux-x86-64-static.nix Checking compiler:"
    echo "CC = $(command -v cc)"
    echo "CXX = $(command -v c++)""
    cc --version
    c++ --version
  '';
}
