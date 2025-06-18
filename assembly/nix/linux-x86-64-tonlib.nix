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
  ];

  buildInputs = with pkgs; [
    pkgsStatic.openssl
    pkgsStatic.zlib
    pkgsStatic.libmicrohttpd.dev
    pkgsStatic.secp256k1
    staticLibs.staticBoost
    staticLibs.staticLibrdkafka
    pkgsStatic.lz4
    (pkgsStatic.libsodium.overrideAttrs (oldAttrs: {
      configureFlags = oldAttrs.configureFlags ++ [ "--disable-pie" ];
      hardeningDisable = oldAttrs.hardeningDisable ++ [ "pie" ];
    }))
  ];

  dontAddStaticConfigureFlags = false;

  cmakeFlags = [
    "-DTON_USE_ABSEIL=ON"
    "-DNIX=ON"
    "-DCMAKE_CXX_FLAGS=-w"
    "-DCMAKE_C_FLAGS=-w"
    "-DCMAKE_CXX_STANDARD=20"
    "-DCPPKAFKA_BUILD_SHARED=0"
    "-DCMAKE_CXX_FLAGS=-Wno-deprecated-declarations -Wno-unused-but-set-variable"
    "-DRDKAFKA_ROOT_DIR=${staticLibs.staticLibrdkafka}"
    "-DCPPKAFKA_RDKAFKA_STATIC_LIB=ON"
    "-DCPPKAFKA_CMAKE_VERBOSE=ON"
  ];

  LDFLAGS = [
    "-static-libgcc" "-static-libstdc++" "-fPIC"
  ];

  ninjaFlags = [
    "tonlibjson" "emulator"
  ];


    postPatch = ''
      substituteInPlace third-party/cppkafka/cmake/FindRdKafka.cmake \
        --replace "try_compile(RdKafka_FOUND" "try_compile(RdKafka_FOUND\n\
      set(_try_compile_dir \"\${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/CMakeTmp\")\n\
      message(STATUS \"try_compile RdKafka_FOUND = \${RdKafka_FOUND}\")\n\
      message(STATUS \"Try compile output dir: \${_try_compile_dir}\")\n\
      if (NOT RdKafka_FOUND)\n\
        file(GLOB _error_logs \"\${_try_compile_dir}/CMakeError.log\")\n\
        foreach (_log \${_error_logs})\n\
          message(STATUS \"---- Begin CMakeError.log ----\")\n\
          file(READ \"\${_log}\" _log_contents)\n\
          string(REPLACE \"\n\" \"\n  \" _log_contents \"\${_log_contents}\")\n\
          message(STATUS \"  \${_log_contents}\")\n\
          message(STATUS \"---- End CMakeError.log ----\")\n\
        endforeach()\n\
      endif()"
    '';

  preConfigure = ''
    echo ">>> linux-x86-64-tonlib.nix Checking compiler:"
    echo "CC = $(command -v cc)"
    echo "CXX = $(command -v c++)"
    cc --version
    c++ --version

    echo "========== FILES IN RDKAFKA =========="
    find ${staticLibs.staticLibrdkafka}
    echo "======================================="
  '';
}
