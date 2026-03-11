#!/bin/bash

set -e

export REPO_ROOT=$(pwd)
export THIRD_PARTY_SRC="${REPO_ROOT}/third-party"

export THIRD_PARTY_DIR="${RUNNER_TEMP:-/tmp}/3pp"
mkdir -p "$THIRD_PARTY_DIR"

export THIRD_PARTY_CLEAR="${RUNNER_TEMP:-/tmp}/3pp_clear"
mkdir -p "$THIRD_PARTY_CLEAR"

NEED_CACHE=false

# All other dependencies (LZ4, Sodium, OpenSSL, Zlib, Libmicrohttpd) are now built 
# over existing cmakefiles in the main project build.

# ==================== librdkafka ====================
# librdkafka is currently not integrated into the main CMake build.
if [ ! -d "$THIRD_PARTY_CLEAR/librdkafka" ]; then
  NEED_CACHE=true
  # Note: This assumes librdkafka source is available or downloaded.
  # If it's not a submodule, it might need to be cloned here if not already present.
  # But the user said "don't need to download existing modules".
  # Assuming it's either already there or handled externally.
  if [ -d "$THIRD_PARTY_SRC/librdkafka" ]; then
    cd "$THIRD_PARTY_SRC/librdkafka"
    ./configure --prefix="$THIRD_PARTY_CLEAR/librdkafka" --enable-static --disable-shared
    make -j$(nproc)
    make install
    echo "Compiled and installed librdkafka"
  else
    echo "librdkafka source not found in $THIRD_PARTY_SRC/librdkafka, skipping build"
  fi
else
  echo "Using existing librdkafka from cache"
fi

export RDKAFKA_ROOT="$THIRD_PARTY_CLEAR/librdkafka"

# ==================== Exported variables summary ====================
echo "export RDKAFKA_ROOT=$RDKAFKA_ROOT"             >> ${RUNNER_TEMP:-/tmp}/3pp/3pp_env.sh
echo "export THIRD_PARTY_DIR=$THIRD_PARTY_DIR"       >> ${RUNNER_TEMP:-/tmp}/3pp/3pp_env.sh
echo "export THIRD_PARTY_CLEAR=$THIRD_PARTY_CLEAR"   >> ${RUNNER_TEMP:-/tmp}/3pp/3pp_env.sh

echo "✅ 3rd party dependencies prepared."

if [ "$NEED_CACHE" = true ]; then
  echo "Need to build 3pp"
  echo "NEED_CACHE=true" >> ${RUNNER_TEMP:-/tmp}/3pp/3pp_status.txt
else
  echo "NEED_CACHE=false" >> ${RUNNER_TEMP:-/tmp}/3pp/3pp_status.txt
fi
