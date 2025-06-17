# export NIX_PATH=nixpkgs=https://github.com/nixOS/nixpkgs/archive/23.05.tar.gz

{ system ? builtins.currentSystem }:

let
  pkgs = import <nixpkgs> {
    inherit system;
  };

  # Переопределяем stdenv с Clang
  stdenv = pkgs.overrideCC pkgs.stdenv pkgs.clang_16;

  # Импорты сторонних библиотек
  microhttpdmy = (import ./microhttpd.nix) { inherit pkgs; };
  staticLibs = import ./static-libs.nix { inherit pkgs; };

in
stdenv.mkDerivation {
  pname = "ton";
  version = "dev-bin";
  src = ./.; # или передай снаружи

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

  NIX_CFLAGS_COMPILE = [
    "-I${microhttpdmy}/usr/local/include"
    "-I${pkgsStatic.openssl}/include"
    "-I${pkgsStatic.zlib}/include"
    "-I${pkgsStatic.libsodium.dev}/include"
    "-I${pkgsStatic.secp256k1}/include"
    "-I${staticLibs.staticBoost}/include"
    "-I${staticLibs.staticLibrdkafka}/include"
    "-I${staticLibs.staticLZ4}/include"
  ];

  LDFLAGS = [
    "-L${microhttpdmy}/usr/local/lib"
    "-L${pkgsStatic.openssl}/lib"
    "-L${pkgsStatic.zlib}/lib"
    "-L${pkgsStatic.libsodium.dev}/lib"
    "-L${pkgsStatic.secp256k1}/lib"
    "-L${staticLibs.staticBoost}/lib"
    "-L${staticLibs.staticLibrdkafka}/lib"
    "-L${staticLibs.staticLZ4}/lib"
    "-static-libgcc" "-static-libstdc++" "-static"
  ];

  preConfigure = ''
    echo ">>> linux-x86-64-static.nix Checking compiler:"
    echo "CC = $(which cc)"
    echo "CXX = $(which c++)"
    cc --version
    c++ --version
  '';
}
