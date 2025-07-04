#!/usr/bin/env bash

set -e
set -o pipefail

# ----------------------
# Variables debug
# ----------------------
if [ -f ${RUNNER_TEMP:-/tmp}/3pp/3pp_env.sh ]; then
  source ${RUNNER_TEMP:-/tmp}/3pp/3pp_env.sh
  echo "✅ Sourced 3pp_env.sh"
else
  echo "⚠️  3pp_env.sh not found, skipping source"
fi

echo "=========== Variables ==========="
echo "LZ4_PATH=$LZ4_PATH"
echo "SODIUM_PATH=$SODIUM_PATH"
echo "OPENSSL_PATH=$OPENSSL_PATH"
echo "ZLIB_PATH=$ZLIB_PATH"
echo "LIBMICROHTTPD_PATH=$LIBMICROHTTPD_PATH"
echo "RDKAFKA_ROOT=$RDKAFKA_ROOT"
echo "================================="

# ----------------------
# Setup ccache
# ----------------------
echo "Setting up ccache..."
mkdir -p ~/.ccache
export CCACHE_DIR=~/.ccache
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
rm -rf .ninja* CMakeCache.txt CMakeFiles

# ----------------------
# Detect OS and set compiler
# ----------------------
if [[ "$OSTYPE" == "darwin"* ]]; then
  echo "Detected macOS"
  export CC="$(brew --prefix llvm@16)/bin/clang"
  export CXX="$(brew --prefix llvm@16)/bin/clang++"
  export OPENSSL_LIBS="$OPENSSL_PATH/lib/libcrypto.a"
else
  echo "Detected Linux"
  export CC=$(which clang-16)
  export CXX=$(which clang++-16)
  export OPENSSL_LIBS="$OPENSSL_PATH/lib64/libcrypto.a"
fi

echo "Using CC: $CC"
echo "Using CXX: $CXX"

# ----------------------
# Configure with CMake
# ----------------------
echo "Configuring project with CMake..."

# Extra linker flags for Linux
LINUX_LINKER_FLAGS=""
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
  LINUX_LINKER_FLAGS="-static-libstdc++ -static-libgcc -latomic"
fi

cmake -GNinja .. \
  -DPORTABLE=1 \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_LINK_SEARCH_START_STATIC=ON \
  -DCMAKE_LINK_SEARCH_END_STATIC=ON \
  -DCMAKE_C_FLAGS="-w" \
  -DCMAKE_CXX_FLAGS="-w -static-libstdc++ -static-libgcc" \
  -DCMAKE_EXE_LINKER_FLAGS="${LINUX_LINKER_FLAGS}" \
  -DTON_USE_PYTHON=1 \
  -DRDKAFKA_ROOT=$RDKAFKA_ROOT \
  -DOPENSSL_FOUND=1 \
  -DOPENSSL_INCLUDE_DIR=$OPENSSL_PATH/include \
  -DOPENSSL_CRYPTO_LIBRARY=$OPENSSL_LIBS \
  -DZLIB_FOUND=1 \
  -DZLIB_INCLUDE_DIR=$ZLIB_PATH \
  -DZLIB_LIBRARIES=$ZLIB_PATH/lib/libz.a \
  -DSODIUM_FOUND=1 \
  -DSODIUM_INCLUDE_DIR=$SODIUM_PATH/include \
  -DSODIUM_LIBRARY_RELEASE=$SODIUM_PATH/lib/libsodium.a \
  -DMHD_FOUND=1 \
  -DMHD_INCLUDE_DIR=$LIBMICROHTTPD_PATH/include \
  -DMHD_LIBRARY=$LIBMICROHTTPD_PATH/lib/libmicrohttpd.a \
  -DLZ4_FOUND=1 \
  -DLZ4_INCLUDE_DIRS=$LZ4_PATH/include \
  -DLZ4_LIBRARIES=$LZ4_PATH/lib/liblz4.a \
#  -DTON_USE_JEMALLOC=ON

echo "✅ CMake configure step succeeded."

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

echo "✅ Build and artifact collection completed successfully."