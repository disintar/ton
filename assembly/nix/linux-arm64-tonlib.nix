# export NIX_PATH=nixpkgs=https://github.com/nixOS/nixpkgs/archive/23.05.tar.gz
{
  pkgs ? import <nixpkgs> { inherit system; }
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
pkgs.llvmPackages_16.stdenv.mkDerivation {
  pname = "ton";
  version = "dev-lib";

  inherit src;

  nativeBuildInputs = with pkgs;
    [
      cmake ninja git pkg-config
    ];

  buildInputs = with pkgs;
    [
      pkgsStatic.openssl
      microhttpdmy
      pkgsStatic.zlib
      pkgsStatic.secp256k1
      staticLibs.staticBoost
      staticLibs.staticLibrdkafka
      pkgsStatic.lz4
      (pkgsStatic.libsodium.overrideAttrs (oldAttrs: {
        # https://github.com/jedisct1/libsodium/issues/292#issuecomment-137135369
        configureFlags = oldAttrs.configureFlags ++ [ " --disable-pie" ];
        hardeningDisable = oldAttrs.hardeningDisable ++ [ "pie" ];
      }))
    ];

  dontAddStaticConfigureFlags = false;

  cmakeFlags = [
    "-DTON_USE_ABSEIL=OFF"
    "-DNIX=ON"
    "-DMHD_FOUND=1"
    "-DMHD_INCLUDE_DIR=${microhttpdmy}/usr/local/include"
    "-DMHD_LIBRARY=${microhttpdmy}/usr/local/lib/libmicrohttpd.a"
    "-DCMAKE_CXX_FLAGS=-w"
    "-DCMAKE_C_FLAGS=-w"
    "-DCMAKE_CXX_STANDARD=23"
    "-DCMAKE_CXX_FLAGS=-Wno-deprecated-declarations -Wno-unused-but-set-variable"
  ];

  LDFLAGS = [
     "-static-libgcc" "-static-libstdc++" "-fPIC" "-fcommon"
  ];

  ninjaFlags = [
    "tonlibjson" "emulator"
  ];
}
