{
  pkgs ? import <nixpkgs> { inherit system; }
, lib ? pkgs.lib
, stdenv ? pkgs.stdenv
, system ? builtins.currentSystem
, src ? ./.
}:

let
  # Override Boost to build static libraries
  staticBoost = pkgs.boost.overrideAttrs (oldAttrs: {
    configureFlags = (oldAttrs.configureFlags or []) ++ ["--with-libraries=all" "--with-toolset=gcc" "--with-icu"];
    buildFlags = (oldAttrs.buildFlags or []) ++ ["link=static"];
  });

  # Override librdkafka to build static libraries
  staticLibrdkafka = pkgs.rdkafka.overrideAttrs (oldAttrs: {
    configureFlags = (oldAttrs.configureFlags or []) ++ ["--enable-static" "--disable-shared"];
  });

  staticLZ4 = pkgs.lz4.overrideAttrs (oldAttrs: rec {
    # Disable shared library building and ensure a static build
    buildInputs = oldAttrs.buildInputs or [];

    # Override the configure flags
    configureFlags = [ "-DBUILD_SHARED_LIBS=OFF" "-DBUILD_STATIC_LIBS=ON" ];
  });
in
{
  inherit staticBoost staticLibrdkafka staticLZ4;
}