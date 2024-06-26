{ pkgs }:

let
    # Override Boost to build static libraries
    staticBoost = pkgs.boost.overrideAttrs (oldAttrs: {
      configureFlags = oldAttrs.configureFlags or [] ++ ["--with-libraries=all" "--with-toolset=gcc" "--with-icu" "--with-locale"];
      buildFlags = oldAttrs.buildFlags or [] ++ ["link=static"];
    });

    # Override librdkafka to build static libraries
    staticLibrdkafka = pkgs.rdkafka.overrideAttrs (oldAttrs: {
      configureFlags = oldAttrs.configureFlags or [] ++ ["--enable-static" "--disable-shared"];
    });

    # Static lz4
    staticLz4 = pkgs.lz4.overrideAttrs (oldAttrs: {
      buildFlags = oldAttrs.buildFlags or [] ++ ["STATIC=1"];
    });
in
{
  inherit staticBoost staticLibrdkafka staticLz4;
}
