#!/usr/bin/env bash
set -e

# Prepare build dir
if [ ! -d "build" ]; then
  mkdir build
  cd build
else
  cd build
  rm -rf .ninja* CMakeCache.txt
fi
buildir=$(pwd)

brew install ninja pkg-config automake libtool autoconf texinfo wget ccache  llvm@16 boost

mkdir -p ~/.ccache
export CCACHE_DIR=~/.ccache
ccache -M 0

export CC="$(brew --prefix llvm@16)/bin/clang"
export CXX="$(brew --prefix llvm@16)/bin/clang++"

mkdir -p ../3pp
# 3pp root
THIRD=../3pp
mkdir -p $THIRD

# librdkafka
if [ ! -d "$THIRD/librdkafka" ]; then
  git clone https://github.com/confluentinc/librdkafka.git $THIRD/librdkafka
  pushd $THIRD/librdkafka
    ./configure --prefix="$(pwd)/install" --enable-static --disable-shared
    make -j$(sysctl -n hw.ncpu)
    make install
  popd
fi
rdkafkaRoot=$(cd $THIRD/librdkafka/install && pwd)

# lz4
if [ ! -d "$THIRD/lz4" ]; then
  git clone https://github.com/lz4/lz4.git $THIRD/lz4
  pushd $THIRD/lz4
    git checkout v1.9.4
    make -j$(sysctl -n hw.ncpu)
  popd
fi
lz4Path=$(cd $THIRD/lz4 && pwd)

# libsodium
if [ ! -d "$THIRD/libsodium" ]; then
  wget -O $THIRD/libsodium-1.0.18.tar.gz https://github.com/jedisct1/libsodium/releases/download/1.0.18-RELEASE/libsodium-1.0.18.tar.gz
  mkdir -p $THIRD/libsodium
  tar xf $THIRD/libsodium-1.0.18.tar.gz -C $THIRD/libsodium --strip-components=1
  pushd $THIRD/libsodium
    ./configure --with-pic --enable-static
    make -j$(sysctl -n hw.ncpu)
  popd
fi
sodiumPath=$(cd $THIRD/libsodium && pwd)

# OpenSSL 3
if [ ! -d "$THIRD/openssl_3" ]; then
  git clone https://github.com/openssl/openssl $THIRD/openssl_3
  pushd $THIRD/openssl_3
    git checkout openssl-3.1.4
    ./config
    make build_libs -j$(sysctl -n hw.ncpu)
  popd
fi
opensslPath=$(cd $THIRD/openssl_3 && pwd)

# zlib
if [ ! -d "$THIRD/zlib" ]; then
  git clone https://github.com/madler/zlib.git $THIRD/zlib
  pushd $THIRD/zlib
    ./configure --static
    make -j$(sysctl -n hw.ncpu)
  popd
fi
zlibPath=$(cd $THIRD/zlib && pwd)

# libmicrohttpd
if [ ! -d "$THIRD/libmicrohttpd" ]; then
  wget -O $THIRD/libmicrohttpd-1.0.1.tar.gz https://ftpmirror.gnu.org/libmicrohttpd/libmicrohttpd-1.0.1.tar.gz
  mkdir -p $THIRD/libmicrohttpd
  tar xf $THIRD/libmicrohttpd-1.0.1.tar.gz -C $THIRD/libmicrohttpd --strip-components=1
  pushd $THIRD/libmicrohttpd
    ./configure --enable-static --disable-tests --disable-benchmark --disable-shared --disable-https --with-pic
    make -j$(sysctl -n hw.ncpu)
  popd
fi
libmicrohttpdPath=$(cd $THIRD/libmicrohttpd && pwd)


# CMake configure
cmake -GNinja .. \
  -DPORTABLE=1 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="-w -I${libmicrohttpdPath}/src/include" \
  -DCMAKE_CXX_FLAGS="-w -I${libmicrohttpdPath}/src/include" \
  -DOPENSSL_FOUND=1 \
  -DOPENSSL_INCLUDE_DIR=${opensslPath}/include \
  -DOPENSSL_CRYPTO_LIBRARY=${opensslPath}/libcrypto.a \
  -DZLIB_FOUND=1 \
  -DZLIB_INCLUDE_DIR=${zlibPath} \
  -DZLIB_LIBRARIES=${zlibPath}/libz.a \
  -DSODIUM_FOUND=1 \
  -DSODIUM_INCLUDE_DIR=${sodiumPath}/src/libsodium/include \
  -DSODIUM_LIBRARY_RELEASE=${sodiumPath}/src/libsodium/.libs/libsodium.a \
  -DMHD_FOUND=1 \
  -DMHD_INCLUDE_DIR=${libmicrohttpdPath}/src/include \
  -DMHD_LIBRARY=${libmicrohttpdPath}/src/microhttpd/.libs/libmicrohttpd.a \
  -DLZ4_FOUND=1 \
  -DLZ4_INCLUDE_DIRS=${lz4Path}/lib \
  -DLZ4_LIBRARIES=${lz4Path}/lib/liblz4.a \
  -DRDKAFKA_ROOT=${rdkafkaRoot} \
  -DTON_USE_PYTHON=1

ninja -j$(sysctl -n hw.ncpu) python_ton

mkdir -p ../artifacts
mv ./tvm-python/*.so ../artifacts/
