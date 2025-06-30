#!/bin/bash

set -e

export THIRD_PARTY_DIR="/tmp/3pp"
mkdir -p "$THIRD_PARTY_DIR"

# ==================== LZ4 ====================
if [ ! -d "$THIRD_PARTY_DIR/lz4" ]; then
  git clone https://github.com/lz4/lz4.git "$THIRD_PARTY_DIR/lz4"
  cd "$THIRD_PARTY_DIR/lz4"
  export LZ4_PATH=$(pwd)
  git checkout v1.9.4
  CFLAGS="-fPIC" make -j$(nproc)
  echo "Compiled LZ4"
else
  export LZ4_PATH="$THIRD_PARTY_DIR/lz4"
  echo "Using compiled LZ4"
fi

# ==================== Libsodium ====================
if [ ! -d "$THIRD_PARTY_DIR/libsodium" ]; then
  mkdir -p "$THIRD_PARTY_DIR/libsodium"
  wget -O "$THIRD_PARTY_DIR/libsodium/libsodium-1.0.18.tar.gz" https://github.com/jedisct1/libsodium/releases/download/1.0.18-RELEASE/libsodium-1.0.18.tar.gz
  cd "$THIRD_PARTY_DIR/libsodium"
  tar xf libsodium-1.0.18.tar.gz
  cd libsodium-1.0.18
  export SODIUM_PATH=$(pwd)
  ./configure --with-pic --enable-static
  make -j$(nproc)
  echo "Compiled libsodium"
else
  export SODIUM_PATH="$THIRD_PARTY_DIR/libsodium/libsodium-1.0.18"
  echo "Using compiled libsodium"
fi

# ==================== OpenSSL ====================
if [ ! -d "$THIRD_PARTY_DIR/openssl_3" ]; then
  git clone https://github.com/openssl/openssl "$THIRD_PARTY_DIR/openssl_3"
  cd "$THIRD_PARTY_DIR/openssl_3"
  export OPENSSL_PATH=$(pwd)
  git checkout openssl-3.1.4
  ./config
  make build_libs -j$(nproc)
  echo "Compiled OpenSSL"
else
  export OPENSSL_PATH="$THIRD_PARTY_DIR/openssl_3"
  echo "Using compiled OpenSSL"
fi

# ==================== Zlib ====================
if [ ! -d "$THIRD_PARTY_DIR/zlib" ]; then
  git clone https://github.com/madler/zlib.git "$THIRD_PARTY_DIR/zlib"
  cd "$THIRD_PARTY_DIR/zlib"
  export ZLIB_PATH=$(pwd)
  ./configure --static
  make -j$(nproc)
  echo "Compiled zlib"
else
  export ZLIB_PATH="$THIRD_PARTY_DIR/zlib"
  echo "Using compiled zlib"
fi

# ==================== Libmicrohttpd ====================
if [ ! -d "$THIRD_PARTY_DIR/libmicrohttpd" ]; then
  mkdir -p "$THIRD_PARTY_DIR/libmicrohttpd"
  wget -O "$THIRD_PARTY_DIR/libmicrohttpd/libmicrohttpd-1.0.1.tar.gz" https://ftpmirror.gnu.org/libmicrohttpd/libmicrohttpd-1.0.1.tar.gz
  cd "$THIRD_PARTY_DIR/libmicrohttpd"
  tar xf libmicrohttpd-1.0.1.tar.gz
  cd libmicrohttpd-1.0.1
  export LIBMICROHTTPD_PATH=$(pwd)
  ./configure --enable-static --disable-tests --disable-benchmark --disable-shared --disable-https --with-pic
  make -j$(nproc)
  echo "Compiled libmicrohttpd"
else
  export LIBMICROHTTPD_PATH="$THIRD_PARTY_DIR/libmicrohttpd/libmicrohttpd-1.0.1"
  echo "Using compiled libmicrohttpd"
fi

# ==================== librdkafka ====================
if [ ! -d "$THIRD_PARTY_DIR/librdkafka" ]; then
  git clone https://github.com/confluentinc/librdkafka.git "$THIRD_PARTY_DIR/librdkafka"
  cd "$THIRD_PARTY_DIR/librdkafka"
  export RDKAFKA_ROOT="$THIRD_PARTY_DIR/librdkafka/install"
  ./configure --prefix="$RDKAFKA_ROOT" --enable-static --disable-shared
  make -j$(nproc)
  make install
  echo "Compiled librdkafka"
else
  export RDKAFKA_ROOT="$THIRD_PARTY_DIR/librdkafka/install"
  echo "Using compiled librdkafka"
fi

# ==================== Exported variables summary ====================
: '
Exported variables for use in CMake or other build scripts:

export LZ4_PATH            - Path to compiled LZ4
export SODIUM_PATH         - Path to compiled libsodium
export OPENSSL_PATH        - Path to compiled OpenSSL
export ZLIB_PATH           - Path to compiled zlib
export LIBMICROHTTPD_PATH  - Path to compiled libmicrohttpd
export RDKAFKA_ROOT        - Installation prefix for librdkafka
export THIRD_PARTY_DIR     - Root directory for all 3rd-party sources
'

echo "✅ All 3rd party dependencies prepared successfully."