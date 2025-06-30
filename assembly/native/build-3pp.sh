#!/bin/bash

set -e

export THIRD_PARTY_DIR="/tmp/3pp"
mkdir -p "$THIRD_PARTY_DIR"

export THIRD_PARTY_CLEAR="/tmp/3pp_clear"
mkdir -p "$THIRD_PARTY_CLEAR"

NEED_CACHE=false

# ==================== LZ4 ====================
if [ ! -d "$THIRD_PARTY_DIR/lz4" ]; then
  NEED_CACHE=true
  git clone https://github.com/lz4/lz4.git "$THIRD_PARTY_DIR/lz4"
  cd "$THIRD_PARTY_DIR/lz4"
  git checkout v1.9.4
  make -j$(nproc) PREFIX="$THIRD_PARTY_CLEAR/lz4"
  make install PREFIX="$THIRD_PARTY_CLEAR/lz4"
  echo "Compiled and installed LZ4"
else
  echo "Using existing LZ4 source"
fi

export LZ4_PATH="$THIRD_PARTY_CLEAR/lz4"

# ==================== Libsodium ====================
if [ ! -d "$THIRD_PARTY_DIR/libsodium" ]; then
  NEED_CACHE=true
  mkdir -p "$THIRD_PARTY_DIR/libsodium"
  wget -O "$THIRD_PARTY_DIR/libsodium/libsodium-1.0.18.tar.gz" https://github.com/jedisct1/libsodium/releases/download/1.0.18-RELEASE/libsodium-1.0.18.tar.gz
  cd "$THIRD_PARTY_DIR/libsodium"
  tar xf libsodium-1.0.18.tar.gz
  cd libsodium-1.0.18
  ./configure --with-pic --enable-static --prefix="$THIRD_PARTY_CLEAR/libsodium"
  make -j$(nproc)
  make install
  echo "Compiled and installed libsodium"
else
  echo "Using existing libsodium source"
fi

export SODIUM_PATH="$THIRD_PARTY_CLEAR/libsodium"

# ==================== OpenSSL ====================
if [ ! -d "$THIRD_PARTY_DIR/openssl_3" ]; then
  NEED_CACHE=true
  git clone https://github.com/openssl/openssl "$THIRD_PARTY_DIR/openssl_3"
  cd "$THIRD_PARTY_DIR/openssl_3"
  git checkout openssl-3.1.4
  ./config --prefix="$THIRD_PARTY_CLEAR/openssl" --openssldir="$THIRD_PARTY_CLEAR/openssl"
  make build_libs -j$(nproc)
  echo "Compiled and installed OpenSSL"
else
  echo "Using existing OpenSSL source"
fi

export OPENSSL_PATH="$THIRD_PARTY_CLEAR/openssl"

# ==================== Zlib ====================
if [ ! -d "$THIRD_PARTY_DIR/zlib" ]; then
  NEED_CACHE=true
  git clone https://github.com/madler/zlib.git "$THIRD_PARTY_DIR/zlib"
  cd "$THIRD_PARTY_DIR/zlib"
  ./configure --static --prefix="$THIRD_PARTY_CLEAR/zlib"
  make -j$(nproc)
  make install
  echo "Compiled and installed zlib"
else
  echo "Using existing zlib source"
fi

export ZLIB_PATH="$THIRD_PARTY_CLEAR/zlib"

# ==================== Libmicrohttpd ====================
if [ ! -d "$THIRD_PARTY_DIR/libmicrohttpd" ]; then
  NEED_CACHE=true
  mkdir -p "$THIRD_PARTY_DIR/libmicrohttpd"
  wget -O "$THIRD_PARTY_DIR/libmicrohttpd/libmicrohttpd-1.0.1.tar.gz" https://ftpmirror.gnu.org/libmicrohttpd/libmicrohttpd-1.0.1.tar.gz
  cd "$THIRD_PARTY_DIR/libmicrohttpd"
  tar xf libmicrohttpd-1.0.1.tar.gz
  cd libmicrohttpd-1.0.1
  ./configure --enable-static --disable-tests --disable-benchmark --disable-shared --disable-https --with-pic --prefix="$THIRD_PARTY_CLEAR/libmicrohttpd"
  make -j$(nproc)
  make install
  echo "Compiled and installed libmicrohttpd"
else
  echo "Using existing libmicrohttpd source"
fi

export LIBMICROHTTPD_PATH="$THIRD_PARTY_CLEAR/libmicrohttpd"

# ==================== librdkafka ====================
if [ ! -d "$THIRD_PARTY_DIR/librdkafka" ]; then
  NEED_CACHE=true
  git clone https://github.com/confluentinc/librdkafka.git "$THIRD_PARTY_DIR/librdkafka"
  cd "$THIRD_PARTY_DIR/librdkafka"
  ./configure --prefix="$THIRD_PARTY_CLEAR/librdkafka" --enable-static --disable-shared
  make -j$(nproc)
  make install
  echo "Compiled and installed librdkafka"
else
  echo "Using existing librdkafka source"
fi

export RDKAFKA_ROOT="$THIRD_PARTY_CLEAR/librdkafka"

# ==================== Exported variables summary ====================
echo "export LZ4_PATH=$LZ4_PATH"                     >> /tmp/3pp/3pp_env.sh
echo "export SODIUM_PATH=$SODIUM_PATH"               >> /tmp/3pp/3pp_env.sh
echo "export OPENSSL_PATH=$OPENSSL_PATH"             >> /tmp/3pp/3pp_env.sh
echo "export ZLIB_PATH=$ZLIB_PATH"                   >> /tmp/3pp/3pp_env.sh
echo "export LIBMICROHTTPD_PATH=$LIBMICROHTTPD_PATH" >> /tmp/3pp/3pp_env.sh
echo "export RDKAFKA_ROOT=$RDKAFKA_ROOT"            >> /tmp/3pp/3pp_env.sh
echo "export THIRD_PARTY_DIR=$THIRD_PARTY_DIR"      >> /tmp/3pp/3pp_env.sh
echo "export THIRD_PARTY_CLEAR=$THIRD_PARTY_CLEAR"  >> /tmp/3pp/3pp_env.sh

echo "✅ All 3rd party dependencies prepared and installed into $THIRD_PARTY_CLEAR."

if [ "$NEED_CACHE" = true ]; then
  echo "Need to build 3pp"
  echo "NEED_CACHE=true" >> /tmp/3pp/3pp_status.txt
else
  echo "NEED_CACHE=false" >> /tmp/3pp/3pp_status.txt
fi