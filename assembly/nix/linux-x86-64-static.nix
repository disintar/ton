{ pkgs ? import <nixpkgs> { inherit system; }
, lib ? pkgs.lib
, stdenv ? pkgs.stdenv
, system ? builtins.currentSystem
, src ? ./.
}:

let
  lib          = pkgs.lib;
  stdenv       = pkgs.overrideCC pkgs.stdenv pkgs.clang_16;
  microhttpdmy = import ./microhttpd.nix { inherit pkgs; };
  staticLibs   = import ./static-libs.nix { inherit pkgs; };
in

stdenv.mkDerivation {
  pname    = "ton";
  version  = "dev-bin";
  inherit src;

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
    libmicrohttpd.dev
    staticLibs.staticLibrdkafka
    staticLibs.staticLZ4
  ];

  makeStatic = true;
  doCheck     = true;

  LDFLAGS = [
    "-static-libgcc" "-static-libstdc++" "-fPIC" "-pthread"
  ];

  cmakeFlags = [
    "-DTON_USE_ABSEIL=OFF"
    "-DNIX=ON"
    "-DBUILD_SHARED_LIBS=OFF"
    "-DCMAKE_LINK_SEARCH_START_STATIC=ON"
    "-DCMAKE_LINK_SEARCH_END_STATIC=ON"
    "-DMHD_FOUND=1"
    "-DMHD_INCLUDE_DIR=${microhttpdmy}/usr/local/include"
    "-DMHD_LIBRARY=${microhttpdmy}/usr/local/lib/libmicrohttpd.a"
    "-DCMAKE_C_COMPILER=${pkgs.clang_16}/bin/clang"
    "-DCMAKE_CXX_COMPILER=${pkgs.clang_16}/bin/clang++"
    "-DCMAKE_CXX_STANDARD=23"
    "-DCMAKE_CXX_FLAGS=-w"
    "-DCMAKE_C_FLAGS=-w"
    "-DCMAKE_CTEST_ARGUMENTS=--timeout;1800"
  ];

  preConfigure = ''
    echo ">>> linux-x86-64-static.nix Checking compiler:"
    echo "CC = $(command -v cc)"
    echo "CXX = $(command -v c++)"
    cc --version
    c++ --version
  '';
}
