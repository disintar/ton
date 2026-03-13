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
  echo "CC=$(xcrun -find clang)"  >> $GITHUB_ENV
  echo "CXX=$(xcrun -find clang++)" >> $GITHUB_ENV
else
  echo "Detected Linux"
  # Detect available clang version (prefer 16, then 18, 20, 17, 19; fallback to default clang)
  find_clang() {
    for v in 16 18 20 17 19; do
      if command -v "clang-${v}" >/dev/null 2>&1 && command -v "clang++-${v}" >/dev/null 2>&1; then
        echo "${v}"
        return 0
      fi
    done
    if command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1; then
      echo "default"
      return 0
    fi
    return 1
  }
  v=$(find_clang) || { echo "No suitable clang found in PATH" >&2; exit 1; }
  if [ "$v" = "default" ]; then
    export CC=$(command -v clang)
    export CXX=$(command -v clang++)
  else
    export CC=$(command -v clang-"$v")
    export CXX=$(command -v clang++-"$v")
  fi
fi

echo "Using CC: $CC"
echo "Using CXX: $CXX"
echo "RDKAFKA_ROOT: ${RDKAFKA_ROOT}"
if [ -n "${RDKAFKA_ROOT}" ]; then
  ls -la "${RDKAFKA_ROOT}" || true
  find "${RDKAFKA_ROOT}" -maxdepth 2 \( -type d -o -type f \) | sort || true
fi

# ----------------------
# Configure with CMake
# ----------------------
echo "Configuring project with CMake..."

# Extra linker flags for Linux
LINUX_LINKER_FLAGS=""
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
  LINUX_LINKER_FLAGS="-static-libstdc++ -static-libgcc -latomic"
  LINUX_LINKER_FLAGS_NOATOMIC="-static-libstdc++ -static-libgcc"
fi

# Help CMake find Homebrew GNU readline on macOS (avoid linking to libedit)
EXTRA_CMAKE_ARGS=""
if [[ "$OSTYPE" == "darwin"* ]]; then
  # SDKROOT=$(xcrun --show-sdk-path)
  if brew ls --versions readline >/dev/null 2>&1; then
    READLINE_PREFIX="$(brew --prefix readline)"
    EXTRA_CMAKE_ARGS="-DCMAKE_PREFIX_PATH=${READLINE_PREFIX}"
    echo "Using Homebrew readline at ${READLINE_PREFIX}"
  fi
fi

cmake -GNinja .. \
  -DPORTABLE=1 \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_LINK_SEARCH_START_STATIC=ON \
  -DCMAKE_LINK_SEARCH_END_STATIC=ON \
  -DCMAKE_C_FLAGS="-w ${LINUX_LINKER_FLAGS_NOATOMIC} -fPIC -pthread" \
  -DCMAKE_CXX_FLAGS="-w -fPIC -pthread ${LINUX_LINKER_FLAGS}" \
  -DCMAKE_EXE_LINKER_FLAGS="${LINUX_LINKER_FLAGS}" \
  -DTON_USE_PYTHON=1 \
  -DRdKafka_ROOT=$RDKAFKA_ROOT \
  ${EXTRA_CMAKE_ARGS} \
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
