#!/bin/sh
# Build the Android proxy smoke test (run it inside a Lepton container).
set -e
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-/opt/android-ndk}}"
CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang++"
[ -x "$CC" ] || { echo "NDK clang++ not found at $CC" >&2; exit 1; }
mkdir -p build/android
"$CC" -O2 -fPIE -pie -static-libstdc++ \
    -o build/android/cream_selftest android/selftest.cpp -ldl -llog
echo "built build/android/cream_selftest"
