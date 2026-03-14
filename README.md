# AutoOMPPass

An out-of-tree **LLVM pass** designed to automatically parallelize loops and regions using OpenMP.

## Usage

Run optimized program.
```bash
./scripts/run.sh input.cpp
```

Get pass remarks.
```bash
./scripts/check-pass.sh input.cpp
```

### Manual Run
If you prefer to run the steps manually:

**1.** Compile to IR
```bash
clang -emit-llvm -g -O1 -S input.cpp -o input.ll
```

**2.** Build and run the pass
```bash
meson setup build
ninja -C build

opt -disable-output input.ll \
    --load-pass-plugin ./build/AutoOMPPass.* \
    -passes=auto-omp \
    --pass-remarks-analysis=auto-omp
```

## Tests

```bash
meson test -C build
```