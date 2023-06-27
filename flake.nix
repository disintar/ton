{
  inputs = {
    nixpkgs-stable.url = "github:nixos/nixpkgs/nixos-23.05";
    nixpkgs-trunk.url = "github:nixos/nixpkgs";
    flake-compat = {
      url = "github:edolstra/flake-compat";
      flake = false;
    };
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs-stable, nixpkgs-trunk, flake-compat, flake-utils }:
    let
      ton = { host, pkgs ? host, stdenv ? pkgs.stdenv, staticGlibc ? false
        , staticMusl ? false, staticExternalDeps ? staticGlibc }:
        with host.lib;
        stdenv.mkDerivation {
          pname = "ton";
          version = "dev";

          src = ./.;

          nativeBuildInputs = with host;
            [ cmake ninja pkg-config git ] ++
            optionals stdenv.isLinux [ dpkg rpm createrepo_c pacman ];
          buildInputs = with pkgs;
          # at some point nixpkgs' pkgsStatic will build with static glibc
          # then we can skip these manual overrides
          # and switch between pkgsStatic and pkgsStatic.pkgsMusl for static glibc and musl builds
            if !staticExternalDeps then [
              openssl_1_1
              zlib
              libmicrohttpd
              libsodium
              secp256k1
            ] else
              [
                (openssl_1_1.override { static = true; }).dev
                (zlib.override { shared = false; }).dev
            ]
            ++ optionals (!stdenv.isDarwin) [
              pkgsStatic.libmicrohttpd.dev
              (pkgsStatic.libsodium.overrideAttrs (oldAttrs: {
                # https://github.com/jedisct1/libsodium/issues/292#issuecomment-137135369
                configureFlags = oldAttrs.configureFlags ++ [ " --disable-pie" ];
                hardeningDisable = oldAttrs.hardeningDisable ++ [ "pie" ];
              }))
              pkgsStatic.secp256k1
            ]
            ++ optionals stdenv.isDarwin [ (libiconv.override { enableStatic = true; enableShared = false; }) ]
            ++ optionals stdenv.isDarwin (forEach [ libmicrohttpd.dev libsodium.dev secp256k1 gmp.dev nettle.dev (gnutls.override { withP11-kit = false; }).dev libtasn1.dev libidn2.dev libunistring.dev gettext ] (x: x.overrideAttrs(oldAttrs: rec { configureFlags = (oldAttrs.configureFlags or []) ++ [ "--enable-static" "--disable-shared" ]; dontDisableStatic = true; })))
            ++ optionals staticGlibc [ glibc.static ];

          dontAddStaticConfigureFlags = stdenv.isDarwin;

          cmakeFlags = [ "-DTON_USE_ABSEIL=OFF" "-DNIX=ON" ] ++ optionals staticMusl [
            "-DCMAKE_CROSSCOMPILING=OFF" # pkgsStatic sets cross
          ] ++ optionals (staticGlibc || staticMusl) [
            "-DCMAKE_LINK_SEARCH_START_STATIC=ON"
            "-DCMAKE_LINK_SEARCH_END_STATIC=ON"
          ];

          LDFLAGS = optional staticExternalDeps (concatStringsSep " " [
            (optionalString stdenv.cc.isGNU "-static-libgcc")
            (optionalString stdenv.isDarwin "-framework CoreFoundation")
            "-static-libstdc++"
          ]);

          GIT_REVISION = if self ? rev then self.rev else "dirty";
          GIT_REVISION_DATE = (builtins.concatStringsSep "-" (builtins.match "(.{4})(.{2})(.{2}).*" self.lastModifiedDate)) + " " + (builtins.concatStringsSep ":" (builtins.match "^........(.{2})(.{2})(.{2}).*" self.lastModifiedDate));

          postInstall = ''
            moveToOutput bin "$bin"
          '';

          preFixup = optionalString stdenv.isDarwin ''
            if [[ -n "$bin" ]]; then
              for fn in "$bin"/bin/*; do
                echo "Fixing libc++ in $fn"
                install_name_tool -change "$(otool -L "$fn" | grep libc++.1 | cut -d' ' -f1 | xargs)" libc++.1.dylib "$fn"
                install_name_tool -change "$(otool -L "$fn" | grep libc++abi.1 | cut -d' ' -f1 | xargs)" libc++abi.dylib "$fn"
              done
            fi

            if [[ -n "$out" ]]; then
              for fn in "$out"/lib/*.dylib; do
                echo "Fixing libc++ in $fn"
                install_name_tool -change "$(otool -L "$fn" | grep libc++.1 | cut -d' ' -f1 | xargs)" libc++.1.dylib "$fn"
                install_name_tool -change "$(otool -L "$fn" | grep libc++abi.1 | cut -d' ' -f1 | xargs)" libc++abi.dylib "$fn"
              done
            fi
          '';

          outputs = [ "bin" "out" ];
        };
      tonPython = ton: python: ton.overrideAttrs (previousAttrs:
        {
          buildInputs = previousAttrs.buildInputs ++ [ python ];

          cmakeFlags = previousAttrs.cmakeFlags ++ [
            "-DTON_USE_PYTHON=1"
            "-DPython_ROOT_DIR=${python}"
            "-DPython_EXECUTABLE=${python.pythonForBuild.interpreter}"
            "-DPython_INCLUDE_DIR=${python}/include/python${python.pythonVersion}"
            "-DPython_LIBRARY=${python}/lib/python${python.pythonVersion}"
          ];

          ninjaFlags = "python_ton";

          installPhase = ''
            runHook preInstall
            cmake --install . --component tvm-python
            runHook postInstall
          '';

          outputs = [ "out" ];
        });
      hostPkgs = system:
        import nixpkgs-stable {
          inherit system;
          config = {
            permittedInsecurePackages = [ "openssl-1.1.1u" ];
          };
          overlays = [
            (self: super: {
              zchunk = nixpkgs-trunk.legacyPackages.${system}.zchunk;
            })
          ];
        };
    in with flake-utils.lib;
    (nixpkgs-stable.lib.recursiveUpdate
      (eachSystem (with system; [ x86_64-linux aarch64-linux ]) (system:
        let
          host = hostPkgs system;
          # look out for https://github.com/NixOS/nixpkgs/issues/129595 for progress on better infra for this
          #
          # nixos 19.09 ships with glibc 2.27
          # we could also just override glibc source to a particular release
          # but then we'd need to port patches as well
          nixos1909 = (import (builtins.fetchTarball {
            url = "https://channels.nixos.org/nixos-19.09/nixexprs.tar.xz";
            sha256 = "1vp1h2gkkrckp8dzkqnpcc6xx5lph5d2z46sg2cwzccpr8ay58zy";
          }) { inherit system; });
          glibc227 = nixos1909.glibc // { pname = "glibc"; };
          stdenv227 = let
            cc = host.wrapCCWith {
              cc = nixos1909.buildPackages.gcc-unwrapped;
              libc = glibc227;
              bintools = host.binutils.override { libc = glibc227; };
            };
          in (host.overrideCC host.stdenv cc);
          tonOldGlibcPython = ton: python: (tonPython ton python).overrideAttrs(previousAttrs: {
            dontPatchELF = true;
            preFixup = ''
              patchelf --remove-rpath $out/*/*
            '';
          });
        in rec {
          packages = rec {
            ton-normal = ton { inherit host; };
            ton-static = ton {
              inherit host;
              stdenv = host.makeStatic host.stdenv;
              staticGlibc = true;
            };
            ton-musl =
              let pkgs = nixpkgs-stable.legacyPackages.${system}.pkgsStatic;
              in ton {
                inherit host;
                inherit pkgs;
                stdenv =
                  pkgs.gcc12Stdenv; # doesn't build on aarch64-linux w/default GCC 9
                staticMusl = true;
              };
            ton-oldglibc = (ton {
              inherit host;
              stdenv = stdenv227;
              staticExternalDeps = true;
            });
            ton-oldglibc_staticbinaries = host.symlinkJoin {
              name = "ton";
              paths = [ ton-musl.bin ton-oldglibc.out ];
            };
            ton-python-39 = tonOldGlibcPython ton-oldglibc host.python39;
            ton-python-310 = tonOldGlibcPython ton-oldglibc host.python310;
            ton-python-311 = tonOldGlibcPython ton-oldglibc host.python311;
          };
          devShells.default =
            host.mkShell {
              inputsFrom = [ packages.ton-oldglibc ];
              shellHook = ''
                export cmakeFlags="${host.lib.concatStringsSep " " packages.ton-normal.cmakeFlags}"
              '';
            };
        })) (eachSystem (with system; [ x86_64-darwin aarch64-darwin ]) (system:
          let host = hostPkgs system;
          in rec {
            packages = rec {
              ton-normal = ton { inherit host; };
              ton-static = ton {
                inherit host;
                stdenv = host.makeStatic host.stdenv;
                staticExternalDeps = true;
              };
              ton-staticbin-dylib = host.symlinkJoin {
                name = "ton";
                paths = [ ton-static.bin ton-static.out ];
              };
              ton-python-39 = tonPython ton-static host.python39;
              ton-python-310 = tonPython ton-static host.python310;
              ton-python-311 = tonPython ton-static host.python311;
            };
            devShells.default =
              host.mkShell {
                inputsFrom = [ packages.ton-static ];
                shellHook = ''
                  export cmakeFlags="${host.lib.concatStringsSep " " packages.ton-normal.cmakeFlags}"
                '';
              };
          })));
}