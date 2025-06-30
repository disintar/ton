#!/bin/bash

# ----------------------
# Variables debug
# ----------------------

if [ -f /tmp/3pp/3pp_env.sh ]; then
  source /tmp/3pp/3pp_env.sh
  echo "✅ Sourced /tmp/3pp/3pp_env.sh"
else
  echo "⚠️  /tmp/3pp/3pp_env.sh not found, skipping source"
fi

echo "=========== Variables ==========="
echo "LZ4_PATH=$LZ4_PATH"
echo "SODIUM_PATH=$SODIUM_PATH"
echo "OPENSSL_PATH=$OPENSSL_PATH"
echo "ZLIB_PATH=$ZLIB_PATH"
echo "LIBMICROHTTPD_PATH=$LIBMICROHTTPD_PATH"
echo "RDKAFKA_ROOT=$RDKAFKA_ROOT"
echo "================================="

set -e  # Exit on first error
set -o pipefail

# ----------------------
# Setup ccache
# ----------------------
echo "Setting up ccache..."
mkdir -p ~/.ccache
export CCACHE_DIR=~/.ccache

# Limit ccache size to avoid unexpected cache blowup (adjust if needed)
ccache -M 5G
ccache --show-stats || echo "ccache not installed properly"

# ----------------------
# Prepare build directory
# ----------------------
echo "Preparing build directory..."
if [ ! -d "build" ]; then
  mkdir build
fi
cd build

# Clean up previous CMake/Ninja state
rm -rf .ninja* CMakeCache.txt CMakeFiles

# ----------------------
# Set compilers
# ----------------------
export CC=$(which clang-16)
export CXX=$(which clang++-16)

echo "Using CC: $CC"
echo "Using CXX: $CXX"

# ----------------------
# Configure with CMake
# ----------------------
echo "Configuring project with CMake..."
cmake -GNinja .. \
  -DPORTABLE=1 \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_CXX_FLAGS="-w -Bstatic /usr/lib/gcc/x86_64-linux-gnu/11/libatomic.a -static-libgcc -static-libstdc++ -latomic" \
  -DCMAKE_EXE_LINKER_FLAGS="-static -latomic" \
  -DTON_USE_PYTHON=1 \
  -DRDKAFKA_ROOT=$RDKAFKA_ROOT \
  -DOPENSSL_FOUND=1 \
  -DOPENSSL_INCLUDE_DIR=$OPENSSL_PATH/include \
  -DOPENSSL_CRYPTO_LIBRARY=$OPENSSL_PATH/lib64/libcrypto.a \
  -DZLIB_FOUND=1 \
  -DZLIB_INCLUDE_DIR=$ZLIB_PATH \
  -DZLIB_LIBRARIES=$ZLIB_PATH/lib/libz.a \
  -DSODIUM_FOUND=1 \
  -DSODIUM_INCLUDE_DIR=$SODIUM_PATH//include \
  -DSODIUM_LIBRARY_RELEASE=$SODIUM_PATH/lib/libsodium.a \
  -DMHD_FOUND=1 \
  -DMHD_INCLUDE_DIR=$LIBMICROHTTPD_PATH/include \
  -DMHD_LIBRARY=$LIBMICROHTTPD_PATH/libs/libmicrohttpd.a \
  -DLZ4_FOUND=1 \
  -DLZ4_INCLUDE_DIRS=$LZ4_PATH/lib \
  -DLZ4_LIBRARIES=$LZ4_PATH/lib/liblz4.a

echo "CMake configure step succeeded."

# ----------------------
# Build
# ----------------------
echo "Building targets with Ninja..."
ninja python_ton

# ----------------------
# Collect artifacts
# ----------------------
echo "Collecting artifacts..."
cd ..
mkdir -p artifacts
mv ./build/tvm-python/*.so ./artifacts/

# ----------------------
# Done
# ----------------------
echo "✅ Build and artifact collection completed successfully."
