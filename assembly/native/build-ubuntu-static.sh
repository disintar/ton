#!/bin/bash

mkdir -p ~/.ccache
export CCACHE_DIR=~/.ccache
ccache -M 0
test $? -eq 0 || { echo "ccache not installed"; exit 1; }

if [ ! -d "build" ]; then
  mkdir build
  cd build
  buildir=`pwd`
else
  cd build
  buildir=`pwd`
  rm -rf .ninja* CMakeCache.txt
fi

export CC=$(which clang-16)
export CXX=$(which clang++-16)

mkdir -p ../3pp


if [ ! -d "../3pp/openssl_3" ]; then
  git clone https://github.com/openssl/openssl ../3pp/openssl_3
  cd ../3pp/openssl_3
  opensslPath=`pwd`
  git checkout openssl-3.1.4
  ./config  no-shared
  make build_libs -j$(nproc)
  test $? -eq 0 || { echo "Can't compile openssl_3"; exit 1; }
  cd $buildir
else
  opensslPath=$(pwd)/../3pp/openssl_3
  echo "Using compiled openssl_3"
fi

cmake -GNinja .. \
      -DPORTABLE=1 \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="-w -static-libgcc -latomic -I${libmicrohttpdPath}/src/include" \
      -DBUILD_SHARED_LIBS=OFF \
      -DOPENSSL_FOUND=1 \Add commentMore actions \
      -DOPENSSL_INCLUDE_DIR=$opensslPath/include \
      -DOPENSSL_CRYPTO_LIBRARY=$opensslPath/libcrypto.a \
      -DCMAKE_CXX_FLAGS="-w -I${libmicrohttpdPath}/src/include -Bstatic /usr/lib/gcc/x86_64-linux-gnu/11/libatomic.a -static-libgcc -static-libstdc++ -latomic" \
      -DCMAKE_EXE_LINKER_FLAGS="-static -latomic" \
      -DTON_USE_PYTHON=1


test $? -eq 0 || { echo "Can't configure ton"; exit 1; }

ninja libtonlibjson.so libemulator.so python_ton

cd ..
mkdir artifacts
mv ./build/tvm-python/*.so ./artifacts
