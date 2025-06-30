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

BASEDIR=`pwd`

# ----------------------
# Build OPENSSL
# ----------------------
echo "Build Openssl..."
mkdir -p /tmp/3pp || echo "3pp exists"

if [ ! -d "/tmp/3pp/openssl_3" ]; then
  git clone https://github.com/openssl/openssl /tmp/3pp/openssl_3
  cd /tmp/3pp/openssl_3

  OPENSSL_PATH=`pwd`

  git checkout openssl-3.1.4
  ./config

  make build_libs -j$(nproc)
  test $? -eq 0 || { echo "Can't compile openssl_3"; exit 1; }
  cd $BASEDIR
else
  OPENSSL_PATH=/tmp/3pp/openssl_3
  echo "Using compiled openssl_3"
fi

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
  -DOPENSSL_FOUND=1 \
  -DOPENSSL_INCLUDE_DIR="$OPENSSL_PATH/include" \
  -DOPENSSL_CRYPTO_LIBRARY="$OPENSSL_PATH/libcrypto.a" \
  -DCMAKE_C_FLAGS="-w -static-libgcc -latomic" \
  -DCMAKE_CXX_FLAGS="-w -Bstatic /usr/lib/gcc/x86_64-linux-gnu/11/libatomic.a -static-libgcc -static-libstdc++ -latomic" \
  -DCMAKE_EXE_LINKER_FLAGS="-static -latomic" \
  -DTON_USE_PYTHON=1

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