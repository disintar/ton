# export NIX_PATH=nixpkgs=https://github.com/nixOS/nixpkgs/archive/23.05.tar.gz

{ pkgs ? import <nixpkgs> { inherit system; }
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
pkgs.llvmPackages_14.stdenv.mkDerivation {
  pname = "ton";
  version = "dev-lib";

  inherit src;

  nativeBuildInputs = with pkgs;
    [ cmake ninja git pkg-config ];

  buildInputs = with pkgs;
    lib.forEach [
      secp256k1 libsodium.dev libmicrohttpd.dev gmp.dev nettle.dev libtasn1.dev libidn2.dev libunistring.dev gettext (gnutls.override { withP11-kit = false; }).dev boostStatic
    ] (x: x.overrideAttrs(oldAttrs: rec { configureFlags = (oldAttrs.configureFlags or []) ++ [ "--enable-static" "--disable-shared" "--disable-tests" ]; dontDisableStatic = true; }))
    ++ [
      darwin.apple_sdk.frameworks.CoreFoundation
      (openssl.override { static = true; }).dev
      (zlib.override { shared = false; }).dev
      (libiconv.override { enableStatic = true; enableShared = false; })
    ];

  dontAddStaticConfigureFlags = true;

  configureFlags = [];

  cmakeFlags = [
    "-DTON_USE_ABSEIL=OFF"
    "-DNIX=ON"
    "-DCMAKE_CXX_FLAGS=-stdlib=libc++"
    "-DCMAKE_OSX_DEPLOYMENT_TARGET:STRING=11.3"
  ];

  LDFLAGS = [
    "-static-libstdc++"
    "-framework CoreFoundation"
  ];

  ninjaFlags = [
    "tonlibjson" "emulator"
  ];

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
}