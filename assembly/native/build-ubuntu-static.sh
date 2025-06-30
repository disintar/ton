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


cmake -GNinja .. \
      -DPORTABLE=1 \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="-w -static-libgcc -latomic -I${libmicrohttpdPath}/src/include" \
      -DBUILD_SHARED_LIBS=OFF \
      -DCMAKE_CXX_FLAGS="-w -I${libmicrohttpdPath}/src/include -Bstatic /usr/lib/gcc/x86_64-linux-gnu/11/libatomic.a -static-libgcc -static-libstdc++ -latomic" \
      -DCMAKE_EXE_LINKER_FLAGS="-static -latomic" \
      -DTON_USE_PYTHON=1


test $? -eq 0 || { echo "Can't configure ton"; exit 1; }

ninja libtonlibjson.so libemulator.so python_ton

cd ..
mkdir artifacts
mv ./build/tvm-python/*.so ./artifacts
