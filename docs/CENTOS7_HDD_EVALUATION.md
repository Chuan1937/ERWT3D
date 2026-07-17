# CentOS 7 HDD Build and Evaluation Guide

## 1. Target environment

The competition-oriented build targets:

- CentOS 7, x86-64
- local HDD as the primary storage target
- approximately 128 GB node memory
- CPU-only execution
- portable x86-64 binary by default

SSD, Windows and GPU-specific tuning are intentionally outside the P4 scope.

## 2. Toolchain

CentOS 7's base compiler is too old for the C++17 implementation. Use GCC 9 or newer from a Software Collections/devtoolset environment, or an equivalent compiler provided by the evaluation node.

Example environment:

```bash
source /opt/rh/devtoolset-9/enable

g++ --version
cmake --version
```

Required dependencies:

```text
C++17 compiler
CMake 3.16 or newer
LZ4 development library
ZFP development library
POSIX threads
```

## 3. Portable release build

The default release build does not use `-march=native`, because the final CPU model is not fixed.

```bash
rm -rf build-release

cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DERWT3D_ENABLE_RZFP=ON \
  -DERWT3D_NATIVE_OPT=OFF

cmake --build build-release -j"$(nproc)"
ctest --test-dir build-release --output-on-failure -j8
```

Machine-local development benchmarks may explicitly enable native tuning:

```bash
-DERWT3D_NATIVE_OPT=ON
```

Do not submit a native-tuned precompiled binary unless the evaluation CPU is known to be compatible.

## 4. Recommended production behavior

The normal reader should use adaptive strategy selection and should not deliberately discard reusable main-payload data between groups.

Recommended benchmark defaults:

```bash
./build-release/erwt3d_bench_rzfp \
  --input DATA.rzfp \
  --output-dir OUTPUT \
  --hdd \
  --read-strategy auto \
  --memory-limit-mb auto \
  --window-cache-mb auto \
  --benchmark-cache-mode stable-auto \
  --group-order official \
  --rounds 3
```

On an approximately 128 GB node, auto memory mode uses at most:

```text
min(32 GiB, 50% of MemAvailable, compressed payload + 6 GiB)
```

This leaves memory for the operating-system page cache, output dirty pages and other node services.

## 5. Strict memory-limited runs

The program must remain functional when the evaluator supplies a limit.

Two GiB example:

```bash
./build-release/erwt3d_bench_rzfp \
  --input DATA.rzfp \
  --output-dir OUTPUT_2G \
  --hdd \
  --memory-limit-mb 2048 \
  --window-cache-mb auto \
  --benchmark-cache-mode cold-group \
  --rounds 3
```

Explicit values are strict upper budgets for accounted output buffers, double I/O windows, compressed-window cache, metadata and reserve. The benchmark reduces output batch size instead of silently exceeding the limit.

## 6. Cache evaluation protocols

Because the organizer has confirmed that cache is cleared before testing but has not yet clarified the granularity, validate both possible procedures.

### Cold group

Best-effort drop of the main payload and clear of the bounded user-space cache before every group:

```bash
--benchmark-cache-mode cold-group
```

This is the worst-case baseline and the primary optimization target.

### Cold round

Clear once before each six-group round and allow cache to accumulate naturally during the round:

```bash
--benchmark-cache-mode cold-round
```

### Warm

Run one unmeasured warm-up round and then measured rounds:

```bash
--benchmark-cache-mode warm
```

### Stable auto

Production behavior. Do not explicitly clear page cache or the user-space cache:

```bash
--benchmark-cache-mode stable-auto
```

`POSIX_FADV_DONTNEED` is a best-effort hint. The organizer's privileged cache-clear procedure remains the authoritative cold-cache mechanism.

## 7. Strategy validation

For every physical HDD and cache mode, compare auto against all forced strategies:

```bash
--read-strategy selective
--read-strategy whole
--read-strategy fullscan
--read-strategy auto
```

Acceptance target:

```text
auto time <= 1.15 × best forced-strategy time
```

For devices below 100 MB/s with a main payload above 8 GiB, auto must not select FullPayloadScan.

## 8. Required result fields

The benchmark CSV records:

- measured sequential bandwidth and seek time
- suspected calibration cache contamination
- cache protocol and request seed
- strict/auto memory budget and batch size
- compressed-window cache capacity, hits, misses and saved bytes
- selected strategy and decision reason
- predicted Selective, Whole and Fullscan times
- requested bytes, physical bytes and read amplification
- I/O, decode, scatter, write and total times
- round mean, median, minimum, maximum and coefficient of variation

## 9. Full-scale gates

### 20 GB

```text
X random <= 23 s on the existing reference HDD
T_composite <= 17 s in the existing validated environment
same-mode CV <= 8%
```

### 50 GB

```text
X random <= 30 s
slow HDD + payload > 8 GiB never selects FullPayloadScan
auto <= 1.15 × best forced strategy
same-mode selected strategy remains stable
```

Cold and warm wall times are not required to be identical. The required property is that a cold cache does not trigger a catastrophic strategy or unnecessary full-payload scan.
