{
  pkgs ? import <nixpkgs> { inherit system; }
, lib ? pkgs.lib
, stdenv ? pkgs.stdenv
, system ? builtins.currentSystem
, src ? ./.
}:

let
  nixos1909 = import (builtins.fetchTarball {
    url = "https://channels.nixos.org/nixos-19.09/nixexprs.tar.xz";
    sha256 = "1vp1h2gkkrckp8dzkqnpcc6xx5lph5d2z46sg2cwzccpr8ay58zy";
  }) { inherit system; };

  glibc227 = nixos1909.glibc // { pname = "glibc"; };

  clangStdenv = let
    cc = pkgs.wrapCCWith {
      cc = pkgs.clang_16;
      libc = glibc227;
      bintools = pkgs.binutils.override { libc = glibc227; };
    };
  in pkgs.overrideCC pkgs.stdenv cc;

  staticLibs = import ./static-libs.nix { inherit pkgs; };

in
clangStdenv.mkDerivation {
  pname = "ton";
  version = "dev-lib";

  inherit src;

  nativeBuildInputs = with pkgs; [
    cmake ninja git pkg-config
  ];

  buildInputs = with pkgs; [
    pkgsStatic.openssl
    pkgsStatic.zlib
    pkgsStatic.libmicrohttpd.dev
    pkgsStatic.secp256k1
    staticLibs.staticBoost
    staticLibs.staticLibrdkafka
    pkgsStatic.lz4
    (pkgsStatic.libsodium.overrideAttrs (oldAttrs: {
      configureFlags = oldAttrs.configureFlags ++ [ "--disable-pie" ];
      hardeningDisable = oldAttrs.hardeningDisable ++ [ "pie" ];
    }))
  ];

  dontAddStaticConfigureFlags = false;

  cmakeFlags = [
    "-DTON_USE_ABSEIL=ON"
    "-DNIX=ON"
    "-DCMAKE_CXX_STANDARD=20"
    "-DCPPKAFKA_BUILD_SHARED=0"
    "-DRDKAFKA_ROOT_DIR=${staticLibs.staticLibrdkafka}"
    "-DCPPKAFKA_RDKAFKA_STATIC_LIB=ON"
    "-DCPPKAFKA_CMAKE_VERBOSE=ON"
    "-DCMAKE_INCLUDE_PATH=${glibc227}/include"
    "-DCMAKE_LIBRARY_PATH=${glibc227}/lib"
  ];

  LDFLAGS = [
    "-static-libgcc" "-static-libstdc++" "-fPIC"
  ];

  ninjaFlags = [
    "tonlibjson" "emulator"
  ];

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

  preConfigure = ''
    echo ">>> linux-x86-64-tonlib.nix Checking compiler:"
    echo "CC = $(command -v cc)"
    echo "CXX = $(command -v c++)"
    cc --version
    c++ --version

    echo "========== FILES IN RDKAFKA =========="
    find ${staticLibs.staticLibrdkafka}
    echo "======================================="
  '';
}
