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
    glibc227.dev
  ];

  buildInputs = with pkgs; [
    pkgsStatic.openssl
    pkgsStatic.zlib
    pkgsStatic.libmicrohttpd.dev
    pkgsStatic.secp256k1
    staticLibs.staticBoost
    pkgs.kernelHeaders
    staticLibs.staticLibrdkafka
    pkgsStatic.lz4
    glibc227
    (pkgsStatic.libsodium.overrideAttrs (oldAttrs: {
      configureFlags = oldAttrs.configureFlags ++ [ "--disable-pie" ];
      hardeningDisable = oldAttrs.hardeningDisable ++ [ "pie" ];
    }))
  ];

  dontAddStaticConfigureFlags = false;

  preConfigure = ''
    echo ">>> linux-x86-64-tonlib.nix Checking compiler:"
    echo "CC = $(command -v cc)"
    echo "CXX = $(command -v c++)"
    cc --version
    c++ --version

    echo "========== FILES IN RDKAFKA =========="
    find ${staticLibs.staticLibrdkafka}
    echo "======================================="

    export CPPFLAGS="-isystem ''${glibcInc}"
    export CFLAGS="$CPPFLAGS -w -mcpu=x86-64 -march=x86-64 -isystem ${glibc227.dev}/include"
    export CXXFLAGS="$CPPFLAGS -w -std=c++23 -Wno-deprecated-declarations -Wno-unused-but-set-variable -mcpu=x86-64 -march=x86-64 -isystem ${glibc227.dev}/include"
    export CPATH="${glibc227.dev}/include"

    echo "CFLAGS / CXXFLAGS"
    echo $CFLAGS
    echo $CXXFLAGS
    '';

  cmakeFlags = [
    "-DTON_USE_ABSEIL=ON"
    "-DNIX=ON"
    "-DCMAKE_CXX_STANDARD=23"
    "-DCMAKE_C_COMPILER=${pkgs.clang_16}/bin/clang"
    "-DCMAKE_CXX_COMPILER=${pkgs.clang_16}/bin/clang++"
    "-DCPPKAFKA_BUILD_SHARED=0"
    "-DCMAKE_CXX_FLAGS=\${CXXFLAGS}"
    "-DCMAKE_SYSTEM_PROCESSOR=x86_64"
    "-DHAVE_ARM64_CRC32C=0"
    "-DHAVE_NEON=0"
    "-DHAVE_F_FULLFSYNC=0"
    "-DCMAKE_C_FLAGS=\${CFLAGS}"
    "-DRDKAFKA_ROOT_DIR=${staticLibs.staticLibrdkafka}"
    "-DCPPKAFKA_RDKAFKA_STATIC_LIB=ON"
    "-DCPPKAFKA_CMAKE_VERBOSE=ON"
  ];

  LDFLAGS = [
    "-static-libgcc" "-static-libstdc++" "-fPIC" "-pthread"
  ];

  ninjaFlags = [
    "tonlibjson" "emulator"
  ];

}
