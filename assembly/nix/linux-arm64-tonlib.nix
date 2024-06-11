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
in
let
  boostStatic = pkgs.boost.overrideAttrs (oldAttrs: {
    buildInputs = (oldAttrs.buildInputs or []) ++ [ pkgsStatic.stdenv ];
    doCheck = false;
    configureFlags = [ "--with-libraries=all" "--with-icu" "--with-serialization" "--with-date_time" "--with-thread" "--with-regex" "--with-filesystem" "--with-program_options" "--with-system" "--with-chrono" "--with-random" "--with-test" ];
  });
in
with import microhttpdmy;
pkgs.llvmPackages_16.stdenv.mkDerivation {
  pname = "ton";
  version = "dev-lib";

  inherit src;

  nativeBuildInputs = with pkgs;
    [
      cmake ninja git pkg-config boostStatic
    ];

  buildInputs = with pkgs;
    [
      pkgsStatic.openssl microhttpdmy pkgsStatic.zlib pkgsStatic.secp256k1
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
  ];

  LDFLAGS = [
     "-static-libgcc" "-static-libstdc++" "-fPIC" "-fcommon"
  ];

  ninjaFlags = [
    "tonlibjson" "emulator"
  ];
}
