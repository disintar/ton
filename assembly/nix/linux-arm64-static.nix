# export NIX_PATH=nixpkgs=https://github.com/nixOS/nixpkgs/archive/23.05.tar.gz

{ pkgs ? import <nixpkgs> { system = builtins.currentSystem; }
, lib ? pkgs.lib
, stdenv ? pkgs.stdenv
, system ? builtins.currentSystem
, src ? ./.
}:

let
  boostStatic = pkgs.boost.overrideAttrs (oldAttrs: {
    buildInputs = (oldAttrs.buildInputs or []) ++ [ pkgsStatic.stdenv ];
    doCheck = false;
    configureFlags = [ "--with-libraries=all" "--with-icu" "--with-serialization" "--with-date_time" "--with-thread" "--with-regex" "--with-filesystem" "--with-program_options" "--with-system" "--with-chrono" "--with-random" "--with-test" ];
  });
in

let
  microhttpdmy = (import ./microhttpd.nix) { inherit pkgs; };
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
        pkgsStatic.openssl
        microhttpdmy
        pkgsStatic.zlib
        pkgsStatic.libsodium.dev
        pkgsStatic.secp256k1
        glibc.static
        boostStatic
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
    "-DBOOST_ROOT=${boostStatic}"
    "-DBOOST_INCLUDEDIR=${boostStatic.dev}/include"
    "-DBOOST_LIBRARYDIR=${boostStatic.lib}/lib"
    "-DBoost_USE_STATIC_LIBS=ON"
  ];

  LDFLAGS = [
     "-static-libgcc" "-static-libstdc++" "-static"
  ];
}
