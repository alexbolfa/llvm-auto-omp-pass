#!/bin/bash
set -e

if [ -z "$1" ]; then
  echo "Usage: $0 <test.cpp>"
  exit 1
fi

cd "$(dirname "$0")/.."

TEST_C="$1"
TEST_LL="${TEST_C%.*}.ll"

# Regenerate .ll if needed
if [ ! -f "$TEST_LL" ] || [ "$TEST_C" -nt "$TEST_LL" ]; then
  clang -emit-llvm -g -S "$TEST_C" -o "$TEST_LL"
fi

if [ "$(uname -s)" == "Darwin" ]; then
  EXT="dylib"
else
  EXT="so"
fi

# Build and run
[ -d build ] || meson setup build
ninja -C build
opt -disable-output "$TEST_LL" \
    --load-pass-plugin "./build/LoopSuggestionPass.$EXT" \
    -passes=loop-suggestions \
    --pass-remarks-analysis=loop-suggestions
