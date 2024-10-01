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

  staticLZ4 = (pkgs.lz4.override { enableStatic = true; enableShared = false; }).dev;
  staticLibiconv = (pkgs.libiconv.override { enableStatic = true; enableShared = false; });
in
{
  inherit staticBoost staticLibrdkafka staticLZ4 staticLibiconv;
}