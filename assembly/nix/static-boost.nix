{ pkgs }:

pkgs.boost.overrideAttrs (oldAttrs: {
  buildInputs = (oldAttrs.buildInputs or []) ++ [ staticPkgs.stdenv ];
  doCheck = false;
  configureFlags = []; # Remove unrecognized flags
  postConfigure = ''
    ./b2 install --prefix=$out --build-dir=build --layout=system link=static threading=multi runtime-link=static
  '';
})
