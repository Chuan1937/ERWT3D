# ERWT3D Benchmark Results

## Test Environment
- CPU: Intel i7-13700F (24 threads, WSL2 on Windows)
- RAM: 64 GB
- OS: Fedora Linux 43 (WSL)
- Compiler: g++ 15.2.1, CMake 3.31
- Data: 256x256x256 float32 synthetic (64 MB raw, 64 MB ERWT3D)

## CUP Real Data
- Small: 801 x 2405 x 2501 (18 GB raw, 19.3 GB ERWT3D, ratio 1.075x)
- Big: 2002 x 2202 x 3001 (50 GB raw)
- Correctness: 100k random samples verified lossless (max_abs_error=0)
- Full benchmarks on CUP data infeasible at current I/O throughput (~389k pread calls per X-slice)

## Key Findings

### Storage
- CUP small: 1.075x (well within 1.5x target)
- 256^3 synthetic: 1.000x (perfectly aligned)

### Axis Balance (256^3, t1, cache0)
- ERWT3D: X/Y/Z = 5.22/3.03/1.92 ms, balance ratio = 2.72
- Raw: X/Y/Z = 19.12/0.70/0.49 ms, balance ratio = 39.02
- ERWT3D is 14x more balanced than raw row-major

### Thread Scaling
- Performance DECREASES with more threads due to mutex contention in cache
- Recommended: single-threaded for small-medium data, fix thread pool for large data

### Bottleneck
- Per-extent pread() generates thousands of syscalls per slice
- Extent merging reduces this but still ~1000+ syscalls for 256^3
- CUP data: ~120k syscalls per Z-slice, ~389k per X-slice
- Next optimization: preadv() batched reads or larger merged extent reads
