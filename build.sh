#!/bin/sh
# Build creamlinux for the host. On SteamOS/Steam Frame (aarch64) this builds
# only the ARM64 library; on x86-64 it builds both 32-bit and 64-bit.
set -e
mkdir -p build output

if [ "$(uname -m)" = "aarch64" ]; then
    mkdir -p build/arm64
    cd build/arm64
    cmake ../.. -DCMAKE_BUILD_TYPE=Release
    cmake --build . -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
    cp -a lib/. ../../output/
    exit 0
fi

mkdir -p build/32 build/64
cd build/32
cmake ../.. -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_C_FLAGS=-m32 -DCMAKE_BUILD_TYPE=Release
cmake --build . -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
cp -a lib/. ../../output/

cd ../..
cd build/64
cmake ../.. -DCMAKE_CXX_FLAGS=-m64 -DCMAKE_C_FLAGS=-m64 -DCMAKE_BUILD_TYPE=Release
cmake --build . -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
cp -a lib/. ../../output/
