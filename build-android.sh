#!/bin/sh
# Build the Android libsteam_api.so proxy used by Steam Frame/Lepton games.
set -e

NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-/opt/android-ndk}}"
if [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
    echo "Android NDK not found. Set ANDROID_NDK_HOME (looked in $NDK)." >&2
    exit 1
fi

BUILD_DIR=build/android
cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=21 \
    -DANDROID_STL=c++_static \
    -DCREAM_ANDROID_PROXY=ON \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo
echo "Android proxy: $BUILD_DIR/lib/libsteam_api.so"
