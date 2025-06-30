#!/bin/bash
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
  -DTON_USE_PYTHON=1
  -DRDKAFKA_ROOT=$RDKAFKA_ROOT \
  -DOPENSSL_FOUND=1 \
  -DOPENSSL_INCLUDE_DIR=$OPENSSL_PATH/include \
  -DOPENSSL_CRYPTO_LIBRARY=$OPENSSL_PATH/libcrypto.a \
  -DZLIB_FOUND=1 \
  -DZLIB_INCLUDE_DIR=$ZLIB_PATH \
  -DZLIB_LIBRARIES=$ZLIB_PATH/libz.a \
  -DSODIUM_FOUND=1 \
  -DSODIUM_INCLUDE_DIR=$SODIUM_PATH/src/libsodium/include \
  -DSODIUM_LIBRARY_RELEASE=$SODIUM_PATH/src/libsodium/.libs/libsodium.a \
  -DMHD_FOUND=1 \
  -DMHD_INCLUDE_DIR=$LIBMICROHTTPD_PATH/src/include \
  -DMHD_LIBRARY=$LIBMICROHTTPD_PATH/src/microhttpd/.libs/libmicrohttpd.a \
  -DLZ4_FOUND=1 \
  -DLZ4_INCLUDE_DIRS=$LZ4_PATH/lib \
  -DLZ4_LIBRARIES=$LZ4_PATH/lib/liblz4.a

echo "CMake configure step succeeded."

# ----------------------
# Build
# ----------------------
echo "Building targets with Ninja..."
ninja libtonlibjson.so libemulator.so python_ton

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