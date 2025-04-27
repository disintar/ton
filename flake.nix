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
        ton = { host, system, kind }:
          let
            p       = host.hostPlatform;
            pkgname = if p.isLinux then
                        if p.isx86      then "linux-x86-64-${kind}.nix"
                        else if p.isAarch64 then "linux-arm64-${kind}.nix"
                        else throw "unsupported platform"
                      else if p.isDarwin then
                        "macos-${kind}.nix"
                      else
                        throw "unsupported platform";
            _   = builtins.trace "Using package: ${pkgname}" null;
            pkg = ./assembly/nix/${pkgname};
          in
          import pkg {
            pkgs   = host;
            inherit system;
            src    = host.nix-gitignore.gitignoreRecursiveSource [] ./.;
          };

        tonPython = host: tonDerivation: python:
          tonDerivation.overrideAttrs (previousAttrs: {
            buildInputs       = (previousAttrs.buildInputs       or []) ++ [ python ];
            nativeBuildInputs = (previousAttrs.nativeBuildInputs or []) ++ [ host.gcc8 ];

            cmakeFlags = (previousAttrs.cmakeFlags or []) ++ [
              "-DTON_USE_PYTHON=1"
              "-DPython_ROOT_DIR=${python}"
              "-DPython_EXECUTABLE=${python.pythonForBuild.interpreter}"
              "-DPython_INCLUDE_DIR=${python}/include/python${python.pythonVersion}"
              "-DPython_LIBRARY=${python}/lib/python${python.pythonVersion}"
            ];

            configureFlags = (previousAttrs.configureFlags or []) ++ [
              "-DCMAKE_C_COMPILER=${host.gcc8}/bin/gcc"
              "-DCMAKE_CXX_COMPILER=${host.gcc8}/bin/g++"
            ];

            doCheck    = false;
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
          overlays = [ ];
        };
    in
    with flake-utils.lib;
    (nixpkgs-stable.lib.recursiveUpdate
      (eachSystem (with system; [ x86_64-linux aarch64-linux ]) (system:
        let
          host = hostPkgs system;
          tonk = kind: ton {
            inherit host;
            inherit system;
            inherit kind;
          };
          tonOldGlibcPython = ton: python: (tonPython host ton python).overrideAttrs (previousAttrs: {
            dontPatchELF = true;
            preFixup = ''
              patchelf --remove-rpath $out/*/*
            '';
          });
        in
        rec {
          packages = rec {
            ton-static = tonk "static";
            ton-tonlib = tonk "tonlib";
            ton-python-39 = tonOldGlibcPython ton-tonlib host.python39;
            ton-python-310 = tonOldGlibcPython ton-tonlib host.python310;
            ton-python-311 = tonOldGlibcPython ton-tonlib host.python311;
            ton-python-312 = tonOldGlibcPython ton-tonlib host.python312;
          };
          devShells.default =
            host.mkShell {
              inputsFrom = [ packages.ton-tonlib ];
              shellHook = ''
                export cmakeFlags="${host.lib.concatStringsSep " " packages.ton-tonlib.cmakeFlags}"
              '';
            };
          defaultPackage = packages.ton-python-39;
        }))
      (eachSystem (with system; [ x86_64-darwin aarch64-darwin ]) (system:
        let
          host = hostPkgs system;
          tonk = kind: ton {
            inherit host;
            inherit system;
            inherit kind;
          };
        in
        rec {
          packages = rec {
            ton-static = tonk "static";
            ton-tonlib = tonk "tonlib";
            ton-python-39 = tonPython host ton-static host.python39;
            ton-python-310 = tonPython host ton-static host.python310;
            ton-python-311 = tonPython host ton-static host.python311;
            ton-python-312 = tonPython host ton-static host.python312;
          };
          devShells.default =
            host.mkShell {
              inputsFrom = [ packages.ton-static ];
              shellHook = ''
                export cmakeFlags="${host.lib.concatStringsSep " " packages.ton-tonlib.cmakeFlags}"
              '';
            };
          defaultPackage = packages.ton-python-39;
        })));
}
