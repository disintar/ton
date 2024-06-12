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
  staticBoost = pkgs.boost;
  staticLibrdkafka = pkgs.rdkafka;
  staticLz4 = pkgs.lz4;
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
      staticBoost
      staticLibrdkafka
      staticLz4
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
  ];

  LDFLAGS = [
     "-static-libgcc" "-static-libstdc++" "-static"
  ];
}
