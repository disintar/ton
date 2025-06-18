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

    staticLibrdkafka = pkgs.rdkafka.overrideAttrs (oldAttrs: {
      configureFlags = (oldAttrs.configureFlags or []) ++ [
        "--enable-static"
        "--disable-shared"
      ];

      postInstall = (oldAttrs.postInstall or "") + ''
        mkdir -p $out/include/librdkafka
        for f in $out/include/*.h; do
          [ -e "$f" ] && mv "$f" "$out/include/librdkafka/"
        done
      '';

      outputs = [ "out" "dev" ];

      postFixup = (oldAttrs.postFixup or "") + ''
        mkdir -p $dev/lib/pkgconfig
        cat > $dev/lib/pkgconfig/rdkafka.pc <<'EOF'
      prefix=$out
      exec_prefix=${prefix}
      libdir=${exec_prefix}/lib
      includedir=${prefix}/include

      Name: rdkafka
      Description: The Apache Kafka C/C++ client library
      Version=${oldAttrs.version}

      Libs: -L${libdir} -lrdkafka
      Cflags: -I${includedir}/librdkafka
      EOF
      '';
    });
  staticLZ4 = (pkgs.lz4.override { enableStatic = true; enableShared = false; }).dev;
  staticLibiconv = (pkgs.libiconv.override { enableStatic = true; enableShared = false; });
in
{
  inherit staticBoost staticLibrdkafka staticLZ4 staticLibiconv;
}