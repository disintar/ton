# export NIX_PATH=nixpkgs=https://github.com/nixOS/nixpkgs/archive/23.05.tar.gz

{ pkgs ? import <nixpkgs> { inherit system; }
, lib ? pkgs.lib
, stdenv ? pkgs.stdenv
, system ? builtins.currentSystem
, src ? ./.
}:
let
  microhttpdmy = (import ./microhttpd.nix) { inherit pkgs; };
in
let
  # Define pkgsStatic with a custom overlay
  pkgsStatic = import <nixpkgs> {
    inherit system;
    overlays = [ (import ./static-overlay.nix) ];
  };
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
      pkgsStatic.openssl microhttpdmy pkgsStatic.zlib pkgsStatic.libsodium.dev pkgsStatic.secp256k1 glibc.static pkgsStatic.boost pkgsStatic.librdkafka
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
  ];

  LDFLAGS = [
     "-static-libgcc" "-static-libstdc++" "-static"
  ];
}
