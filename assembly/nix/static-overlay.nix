self: super: {
  boost = super.boost.overrideAttrs (oldAttrs: {
    buildInputs = (oldAttrs.buildInputs or []) ++ [ super.stdenv ];
    doCheck = false;
    configurePhase = '' ''; # No configure phase
    buildPhase = ''
      ./bootstrap.sh --prefix=$out --with-toolset=clang
      ./b2 install --prefix=$out --build-dir=build --layout=system link=static threading=multi runtime-link=static
    '';
  });

  librdkafka = super.librdkafka.overrideAttrs (oldAttrs: {
    configureFlags = (oldAttrs.configureFlags or []) ++ [ "--enable-static", "--disable-shared" ];
  });
}
