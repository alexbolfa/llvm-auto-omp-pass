#!/bin/bash
set -e

# 1. Setup & Paths
CLANG="clang++"
EXT=$([ "$(uname)" == "Darwin" ] && echo "dylib" || echo "so")
SDK_FLAGS=$([ "$EXT" == "dylib" ] && echo "-isysroot $(xcrun --show-sdk-path)")
OMP_PREFIX="${OMP_PREFIX:-/opt/homebrew/opt/libomp}"

# 2. File Handling
[ -z "$1" ] && { echo "Usage: $0 <file.cpp>"; exit 1; }
SRC="$1"
BIN="$(basename "${SRC%.*}.out")"

# 3. Build Plugin & Compile
[ ! -d build ] && meson setup build --buildtype=release
ninja -C build --quiet

$CLANG $SDK_FLAGS -O1 -fopenmp \
    -I"$OMP_PREFIX/include" -L"$OMP_PREFIX/lib" \
    -fpass-plugin="./build/AutoOMPPass.$EXT" \
    -Rpass-analysis=auto-omp \
    "$SRC" -o "$BIN"

# 4. Execute
echo "=== Output ==="
"./$BIN"