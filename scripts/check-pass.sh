#!/bin/bash
set -e

# 1. Environment & OS Setup
[[ -z "$1" ]] && { echo "Usage: $0 <file.cpp>"; exit 1; }
EXT=$([[ "$(uname)" == "Darwin" ]] && echo "dylib" || echo "so")
SDK_FLAGS=$([[ "$EXT" == "dylib" ]] && echo "-isysroot $(xcrun --show-sdk-path)")

# 2. Files (Strip path from output .ll)
SRC="$1"
LL="$(basename "${SRC%.*}.ll")"

# 3. Build Plugin & IR
[[ ! -d build ]] && meson setup build --buildtype=release
ninja -C build --quiet
clang++ $SDK_FLAGS -g -O1 -ffast-math -emit-llvm -S "$SRC" -o "$LL" 

# 4. Run Pass
opt -disable-output "$LL" \
    -load-pass-plugin "./build/AutoOMPPass.$EXT" \
    -passes=auto-omp \
    --pass-remarks-analysis=auto-omp
