{ system ? builtins.currentSystem }:

let
  pkgs = import <nixpkgs> { inherit system; };

  nixos1909 = import (builtins.fetchTarball {
    url = "https://channels.nixos.org/nixos-19.09/nixexprs.tar.xz";
    sha256 = "1vp1h2gkkrckp8dzkqnpcc6xx5lph5d2z46sg2cwzccpr8ay58zy";
  }) { inherit system; };

  glibc227 = nixos1909.glibc // { pname = "glibc"; };

  cc = pkgs.wrapCCWith {
    cc = nixos1909.buildPackages.gcc-unwrapped;
    libc = glibc227;
    bintools = pkgs.binutils.override { libc = glibc227; };
  };

  stdenv227 = pkgs.overrideCC pkgs.stdenv cc;

  staticLibs = import ./static-libs.nix { inherit pkgs; };

in stdenv227.mkDerivation {
  pname = "ton";
  version = "dev-lib";
  src = ./.; # если используешь в git-репозитории

  nativeBuildInputs = with pkgs; [ cmake ninja git pkg-config ];

  buildInputs = with pkgs; [
    pkgsStatic.openssl
    pkgsStatic.zlib
    pkgsStatic.libmicrohttpd.dev
    pkgsStatic.secp256k1
    pkgsStatic.lz4
    staticLibs.staticBoost
    staticLibs.staticLibrdkafka
    (pkgsStatic.libsodium.overrideAttrs (oldAttrs: {
      configureFlags = oldAttrs.configureFlags ++ [ "--disable-pie" ];
      hardeningDisable = oldAttrs.hardeningDisable ++ [ "pie" ];
    }))
  ];

  dontAddStaticConfigureFlags = false;

  cmakeFlags = [
    "-DTON_USE_ABSEIL=ON"
    "-DNIX=ON"
    "-DCMAKE_C_COMPILER=${cc}/bin/cc"
    "-DCMAKE_CXX_COMPILER=${cc}/bin/c++"
    "-DCMAKE_CXX_STANDARD=20"
    "-DCMAKE_CXX_FLAGS=-Wno-deprecated-declarations -Wno-unused-but-set-variable -w"
    "-DCMAKE_C_FLAGS=-w"
  ];

  LDFLAGS = [
    "-static-libgcc"
    "-static-libstdc++"
    "-fPIC"
  ];

  ninjaFlags = [ "tonlibjson" "emulator" ];

  preConfigure = ''
    echo ">>> linux-x86-64-tonlib.nix Checking compiler:"
    echo "CC = $(which cc)"
    echo "CXX = $(which c++)"
    cc --version
    c++ --version
  '';
}
