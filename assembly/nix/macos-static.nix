# export NIX_PATH=nixpkgs=https://github.com/nixOS/nixpkgs/archive/23.05.tar.gz

{ pkgs ? import <nixpkgs> { inherit system; }
, lib ? pkgs.lib
, stdenv ? pkgs.stdenv
, system ? builtins.currentSystem
, src ? ./.
}:
let
  staticLibs = import ./static-libs.nix { inherit pkgs; };
in
pkgs.llvmPackages_14.stdenv.mkDerivation {
  pname = "ton";
  version = "dev-bin";

  inherit src;

  nativeBuildInputs = with pkgs;
    [ cmake ninja git pkg-config ];

  buildInputs = with pkgs;
   lib.forEach [
        secp256k1 libsodium.dev libmicrohttpd.dev gmp.dev nettle.dev libtasn1.dev libidn2.dev libunistring.dev gettext (gnutls.override { withP11-kit = false; }).dev
      ]
      (x: x.overrideAttrs(oldAttrs: rec { configureFlags = (oldAttrs.configureFlags or []) ++ [ "--enable-static" "--disable-shared" "--disable-tests" ]; dontDisableStatic = true; }))
    ++ [
      darwin.apple_sdk.frameworks.CoreFoundation
      (openssl.override { static = true; }).dev
      (zlib.override { shared = false; }).dev
      (libiconv.override { enableStatic = true; enableShared = false; })
      pkgsStatic.boost
      staticLibs.staticLibrdkafka
      staticLibs.staticLZ4
      staticLibs.staticLibiconv
   ];


  dontAddStaticConfigureFlags = true;
  makeStatic = true;
  doCheck = true;

  configureFlags = [];

  cmakeFlags = [
    "-DTON_USE_ABSEIL=OFF"
    "-DNIX=ON"
    "-DCMAKE_CROSSCOMPILING=OFF"
    "-DCMAKE_LINK_SEARCH_START_STATIC=ON"
    "-DCMAKE_LINK_SEARCH_END_STATIC=ON"
    "-DBUILD_SHARED_LIBS=OFF"
    "-DCMAKE_CXX_FLAGS=-stdlib=libc++"
    "-DCMAKE_OSX_DEPLOYMENT_TARGET:STRING=11.3"
    "-DCMAKE_CTEST_ARGUMENTS=--timeout;1800"
    "-DCMAKE_CXX_FLAGS=-w"
    "-DCMAKE_C_FLAGS=-w"
    "-DCMAKE_CXX_STANDARD=20"
  ];

  LDFLAGS = [
    "-static-libstdc++"
    "-framework CoreFoundation"
  ];

  preConfigure = ''
      echo ">>> macos-static.nix Checking compiler:"
      echo "CC = $(which cc)"
      echo "CXX = $(which c++)"
      cc --version
      c++ --version
  '';

  postInstall = ''
     moveToOutput bin "$bin"
  '';

  preFixup = ''
      if [[ -n "$bin" ]]; then
        for fn in "$bin"/bin/*; do
          echo "Fixing libc++ in $fn"
          install_name_tool -change "$(otool -L "$fn" | grep libc++.1 | cut -d' ' -f1 | xargs)" libc++.1.dylib "$fn"
          install_name_tool -change "$(otool -L "$fn" | grep libc++abi.1 | cut -d' ' -f1 | xargs)" libc++abi.dylib "$fn"
        done
      fi

      if [[ -n "$out" ]]; then
        for fn in "$out"/lib/*.{dylib,so}; do
          echo "Fixing libc++ in $fn"
          install_name_tool -change "$(otool -L "$fn" | grep libc++.1 | cut -d' ' -f1 | xargs)" libc++.1.dylib "$fn"
          install_name_tool -change "$(otool -L "$fn" | grep libc++abi.1 | cut -d' ' -f1 | xargs)" libc++abi.dylib "$fn"
        done
        fi
  '';
  outputs = [ "bin" "out" ];
}