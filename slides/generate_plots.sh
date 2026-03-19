#!/bin/bash

# Exit on error, undefined variables, and pipe failures
set -euo pipefail

# --- Configuration ---
BENCH_SEQ="./build/loop_benchmarks_seq"
BENCH_PAR="./build/loop_benchmarks"
PLOT_SCRIPT="slides/plot_bench.py"
OUT_DIR="slides"
REPS=5  # Increased reps for better median stability

# Ensure output directory exists
mkdir -p "$OUT_DIR"

# 1. Validation: Check if binaries exist
for cmd in "$BENCH_SEQ" "$BENCH_PAR"; do
    if [[ ! -f "$cmd" ]]; then
        echo "Error: Binary $cmd not found. Did you build the project?"
        exit 1
    fi
done

echo "--- 🚀 Starting Benchmarks ---"

# 2. Run Sequential Benchmarks
echo "Running Sequential Baseline (x$REPS)..."
$BENCH_SEQ --benchmark_format=csv \
           --benchmark_repetitions=$REPS \
           --benchmark_min_time=0.1 \
           2>/dev/null > /tmp/sequential.csv

# 3. Run Parallel Benchmarks
echo "Running AutoOMPPass Parallel (x$REPS)..."
$BENCH_PAR --benchmark_format=csv \
           --benchmark_repetitions=$REPS \
           --benchmark_min_time=0.1 \
           2>/dev/null > /tmp/parallel.csv

echo "--- 📊 Generating Plot ---"

# 4. Generate the Plot using 'uv'
# We use 'baseline' and 'optimized' as labels for the script
uv run "$PLOT_SCRIPT" /tmp/sequential.csv /tmp/parallel.csv \
    -o "$OUT_DIR/bench_chart.png"

echo "--- ✅ Success! ---"
echo "View your chart at: $OUT_DIR/bench_chart.png"

# Optional: Clean up temp files
# rm /tmp/sequential.csv /tmp/parallel.csv

