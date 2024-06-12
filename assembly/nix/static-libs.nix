{ pkgs }:

let
  staticBoost = pkgs.boost.overrideAttrs (oldAttrs: {
    buildInputs = (oldAttrs.buildInputs or []) ++ [ pkgs.stdenv ];
    doCheck = false;
    configurePhase = '' ''; # No configure phase
    buildPhase = ''
      ./bootstrap.sh --prefix=$out --with-toolset=clang
      ./b2 install --prefix=$out --build-dir=build --layout=system link=static threading=multi runtime-link=static
    '';
    postInstall = ''
      mv $out/include/boost $out/include/boost-moved
      mv $out/include/boost-moved/* $out/include/
      rmdir $out/include/boost-moved
    '';
  });


  staticLibrdkafka = pkgs.rdkafka.overrideAttrs (oldAttrs: {
    configureFlags = (oldAttrs.configureFlags or []) ++ [ "--enable-static" "--disable-shared" ];
    postInstall = ''
      mkdir -p $out/lib
      mv $oldAttrs.out/lib/* $out/lib/
    '';
  });
in
{
  inherit staticBoost staticLibrdkafka;
}