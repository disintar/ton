# export NIX_PATH=nixpkgs=https://github.com/nixOS/nixpkgs/archive/23.05.tar.gz

{ pkgs ? import <nixpkgs> { inherit system; }
, lib ? pkgs.lib
, stdenv ? pkgs.stdenv
, system ? builtins.currentSystem
, src ? ./.
}:
let
  microhttpdmy = (import ./microhttpd.nix) { inherit pkgs; };
  staticLibs = import ./static-libs.nix { inherit pkgs; };
in
with import microhttpdmy;
stdenv.mkDerivation {
  pname = "ton";
  version = "dev-bin";

  inherit src;

  nativeBuildInputs = with pkgs;
    [
      cmake ninja git pkg-config
    ];

  buildInputs = with pkgs;
    [
      pkgsStatic.openssl microhttpdmy pkgsStatic.zlib pkgsStatic.libsodium.dev pkgsStatic.secp256k1 glibc.static
      staticLibs.staticBoost
      staticLibs.staticLibrdkafka
      staticLibs.staticLz4
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
    "-DCMAKE_CTEST_ARGUMENTS=--timeout;1800"
    "-DCMAKE_CXX_FLAGS=-w"
    "-DCMAKE_C_FLAGS=-w"
    "-DCMAKE_CXX_STANDARD=17"
    "-DCMAKE_CXX_FLAGS=-Wno-deprecated-declarations -Wno-unused-but-set-variable"
  ];

  NIX_CFLAGS_COMPILE = [
    "-I${microhttpdmy}/usr/local/include"
    "-I${pkgsStatic.openssl}/include"
    "-I${pkgsStatic.zlib}/include"
    "-I${pkgsStatic.libsodium.dev}/include"
    "-I${pkgsStatic.secp256k1}/include"
    "-I${staticBoost}/include"
    "-I${staticLibrdkafka}/include"
    "-I${staticLz4}/include"
  ];

  LDFLAGS = [
    "-L${microhttpdmy}/usr/local/lib"
    "-L${pkgsStatic.openssl}/lib"
    "-L${pkgsStatic.zlib}/lib"
    "-L${pkgsStatic.libsodium.dev}/lib"
    "-L${pkgsStatic.secp256k1}/lib"
    "-L${staticBoost}/lib"
    "-L${staticLibrdkafka}/lib"
    "-L${staticLz4}/lib"
    "-static-libgcc" "-static-libstdc++" "-static"
  ];
}