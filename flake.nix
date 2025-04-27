{
  inputs = {
    nixpkgs-stable.url = "github:nixos/nixpkgs/nixos-23.05";
    flake-utils.url    = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs-stable, flake-utils }: let
    # Overlay to force GCC 8.3 for all builds
    myOverlays = [
      (final: prev: {
        gcc           = prev.gcc8;
        gcc-unwrapped = prev.gcc8;
        stdenv        = prev.gcc8.stdenv;
      })
    ];

    # Import TON package based on host and kind
    tonSrc = { host, system, kind }: let
      p = host.hostPlatform;
      pkgname = if p.isLinux then
        if p.isx86 then "linux-x86-64-${kind}.nix"
        else if p.isAarch64 then "linux-arm64-${kind}.nix"
        else throw "unsupported Linux platform"
      else if p.isDarwin then
        "macos-${kind}.nix"
      else
        throw "unsupported platform";
      pkg = ./assembly/nix/${pkgname};
    in import pkg {
      inherit host;
      inherit system;
      pkgs = host;
      src  = host.nix-gitignore.gitignoreRecursiveSource [] ./.;
    };

    # Extend TON to build Python bindings
    tonWithPython = ton: python: ton.overrideAttrs (prev: {
      buildInputs = prev.buildInputs ++ [ python ];
      cmakeFlags = prev.cmakeFlags ++ [
        "-DTON_USE_PYTHON=1"
        "-DPython_ROOT_DIR=${python}"
        "-DPython_EXECUTABLE=${python.pythonForBuild.interpreter}"
        "-DPython_INCLUDE_DIR=${python}/include/python${python.pythonVersion}"
        "-DPython_LIBRARY=${python}/lib/python${python.pythonVersion}"
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

    # Host pkgs with gcc8 overlay
    hostPkgs = system:
      import nixpkgs-stable {
        inherit system;
        overlays = myOverlays;
      };

  in flake-utils.lib.eachDefaultSystem (system: let
      host   = hostPkgs system;
      # create TON builds for 'static' and 'tonlib'
      tonStatic = tonSrc { inherit host system; kind = "static"; };
      tonLib    = tonSrc { inherit host system; kind = "tonlib"; };
      # apply Python extension
      tonStaticPy39  = tonWithPython tonStatic host.python39;
      tonLibPy39     = tonWithPython tonLib    host.python39;
      tonLibPy310    = tonWithPython tonLib    host.python310;
      tonLibPy311    = tonWithPython tonLib    host.python311;
      tonLibPy312    = tonWithPython tonLib    host.python312;
    in {
      packages = {
        "ton-static"    = tonStatic;
        "ton-tonlib"    = tonLib;
        "ton-python-39"  = tonLibPy39;
        "ton-python-310" = tonLibPy310;
        "ton-python-311" = tonLibPy311;
        "ton-python-312" = tonLibPy312;
      };

      devShells.default = host.mkShell {
        inputsFrom = [ packages."ton-tonlib" ];
        shellHook = ''
          export cmakeFlags="${host.lib.concatStringsSep " " packages."ton-tonlib".cmakeFlags}"
        '';
      };

      defaultPackage = packages."ton-python-39";
    }
  );
}