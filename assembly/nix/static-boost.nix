{ pkgs }:

pkgs.boost.overrideAttrs (oldAttrs: {
  buildInputs = (oldAttrs.buildInputs or []) ++ [ pkgs.stdenv ];
  doCheck = false;
  postConfigure = ''
    ./b2 install --prefix=$out --build-dir=build --layout=system link=static threading=multi runtime-link=static
  '';
})