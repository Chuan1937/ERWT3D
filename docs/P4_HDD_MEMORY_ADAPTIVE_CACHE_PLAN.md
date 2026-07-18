# P4: HDD Memory-Adaptive and Cache-Stable I/O

## 1. Scope

P4 targets the expected competition environment:

- CentOS 7, x86-64
- one local HDD as the primary optimization target
- approximately 128 GB node memory
- system page cache cleared before testing, but it is not yet known whether this happens once per six-group round or before every group
- the program must support a strict memory limit when requested, including a 2 GB limit
- SSD, Windows and GPU paths are not part of this phase

P4 must preserve all P2/P3 format, correctness and storage-budget guarantees. It must not change the Raw X auxiliary on-disk format or weaken the 1.45x hard storage limit.

The primary goal is not identical wall time on every disk. The goal is that the automatic reader remains close to the best strategy on each HDD and does not collapse when the cache state, disk bandwidth or memory budget changes.

## 2. Baseline assumptions

The implementation must be robust under both possible official benchmark procedures:

### Cold round

The system page cache is cleared once before the full six-group benchmark, then all groups run continuously.

```text
clear cache
X random
Y random
Z random
X continuous
Y continuous
Z continuous
```

### Cold group

The system page cache is cleared before every individual group.

```text
clear cache -> X random
clear cache -> Y random
clear cache -> Z random
...
```

The implementation must optimize cold-group as the worst-case baseline while retaining natural cache reuse under cold-round. The normal production reader must not actively discard the main RZFP payload after each group.

## 3. Non-negotiable constraints

The following behavior must remain unchanged:

- `FLAG_HAS_RAW_X_AUX`
- Raw X auxiliary layout and metadata
- LZ4 and RZFP Raw X writers
- Raw X bit-exact X-plane reads
- RZFP relative-error limit
- official Z-fastest raw layout
- 1.45x non-bypassable hard storage limit
- transactional append and metadata validation
- existing local CTest suite

Performance regression limits:

- 20 GB X random must remain <= 23 s on the existing reference HDD
- 50 GB X random must remain <= 30 s
- Raw X storage ratios must remain approximately 1.432x and 1.421x for the validated datasets

## 4. Work package A: finish P3 hardening

### A1. Prevent page-cache contamination during device calibration

Files:

- `include/erwt3d/device_profile.hpp`
- `src/device_profile.cpp`

Extend `DeviceCalibrationConfig`:

```cpp
struct DeviceCalibrationConfig {
    uint64_t sequential_region_bytes = 128ULL * 1024 * 1024;
    uint32_t sequential_region_count = 3;
    uint32_t random_probe_count = 64;
    uint64_t random_probe_bytes = 64ULL * 1024;

    bool evict_before_probe = true;
    bool evict_after_probe = true;
};
```

For every sequential and random probe:

1. call `posix_fadvise(..., POSIX_FADV_DONTNEED)` before timing when enabled;
2. run the timed read;
3. call DONTNEED again after timing when enabled.

The strategy model must prefer an underestimated physical bandwidth over a cache-inflated result. If samples differ excessively, use the lower robust statistic rather than the maximum. Record whether the calibration appears cache-contaminated.

Add fields:

```cpp
bool cache_contamination_suspected = false;
double minimum_sequential_mb_s = 0.0;
double maximum_sequential_mb_s = 0.0;
```

### A2. Make the adaptive decision independently testable

Files:

- new: `include/erwt3d/rzfp_strategy.hpp`
- new: `src/rzfp_strategy.cpp`
- `src/rzfp_reader.cpp`

Move cost comparison into a pure function:

```cpp
struct StrategyCostInput {
    uint64_t selective_bytes = 0;
    uint64_t selective_preads = 0;
    uint64_t whole_bytes = 0;
    uint64_t whole_preads = 0;
    uint64_t fullscan_bytes = 0;
    uint64_t fullscan_preads = 0;
    uint64_t decoded_records = 0;
    double sequential_mb_s = 0.0;
    double seek_ms = 0.0;
    double decode_records_per_second = 500000.0;
};

StrategyDecision chooseAdaptiveStrategyFromCosts(
    const StrategyCostInput& input,
    const RzfpAdaptiveConfig& config
);
```

`src/rzfp_reader.cpp` remains responsible for deriving actual byte and pread counts, then calls the pure decision function.

### A3. Correct FullPayloadScan protection semantics

Rules:

- slow device plus large payload is an absolute rejection:
  - `sequential_mb_s < 100`
  - `fullscan_bytes > 8 GiB`
- the 120 s soft limit may only be exceeded if Fullscan is at least 20% faster than the best non-Fullscan strategy
- if Fullscan is rejected, choose the faster of WholeSuperblock and SelectiveLeaf; never always fall back to SelectiveLeaf
- if the top two allowed strategies differ by less than 15%, choose the strategy with lower read volume; tie order is WholeSuperblock, SelectiveLeaf, FullPayloadScan

### A4. Fix strategy pilot recalculation

The pilot must not multiply all complete strategy times by the same scale factor. Recalculate only the I/O component:

```cpp
corrected_total =
    read_bytes / observed_bytes_per_second
    + seek_seconds
    + decode_seconds;
```

The pilot must read up to the configured 256 MiB from a valid continuous payload interval, not merely four superblocks. It must have a hard 3 s time budget. It runs only when the decision is uncertain.

After the pilot, DONTNEED its range. The slow-device absolute Fullscan rejection must remain in force after pilot correction.

### A5. Complete strategy observability

Extend `RzfpReadProfile`:

```cpp
double predicted_selective_seconds = 0.0;
double predicted_whole_seconds = 0.0;
double predicted_fullscan_seconds = 0.0;
double effective_device_mb_s = 0.0;
double pilot_observed_mb_s = 0.0;
std::string strategy_reason;
CachePolicy cache_policy = CachePolicy::StableAuto;
```

The benchmark must output the actual decision reason rather than a placeholder.

### A6. Pre-calibrate before the six-group benchmark

Add a public method:

```cpp
const DeviceProfile& ensureDeviceProfile(
    const DeviceCalibrationConfig& config = {}
);
```

`erwt3d_bench_rzfp` calls it before running X random. This prevents the final CSV from reporting zero device bandwidth merely because the first group used Raw X Aux and never entered the main RZFP strategy path.

Raw X groups must report strategy `raw-x-aux`, not `auto`.

## 5. Work package B: strict and automatic memory budgeting

### B1. Add memory-budget module

Files:

- new: `include/erwt3d/memory_budget.hpp`
- new: `src/memory_budget.cpp`

Public API:

```cpp
enum class MemoryLimitMode {
    Explicit,
    Auto
};

struct MemoryBudget {
    uint64_t total_bytes = 0;
    uint64_t io_buffer_bytes = 0;
    uint64_t output_buffer_bytes = 0;
    uint64_t window_cache_bytes = 0;
    uint64_t reserve_bytes = 0;
    bool automatic = false;
};

uint64_t readLinuxMemAvailableBytes();

MemoryBudget makeMemoryBudget(
    const std::string& value,
    uint64_t payload_bytes,
    uint64_t bytes_per_output_slice,
    uint64_t requested_slice_count
);
```

CLI semantics:

```text
--memory-limit-mb 2048
--memory-limit-mb 8192
--memory-limit-mb 32768
--memory-limit-mb auto
```

Explicit values are strict upper limits. Auto mode uses:

```text
min(
    32 GiB,
    50% of /proc/meminfo MemAvailable,
    payload size + 4 GiB
)
```

with a minimum usable target of 4 GiB when the system permits it. Never reserve all available memory. Leave room for the kernel page cache, output dirty pages and other services.

Suggested split:

- up to 1 GiB for double I/O buffers and codec workspaces
- output buffers sized for the largest safe group batch
- remaining budget for the bounded compressed-window cache
- at least 1 GiB internal reserve in auto mode

### B2. Enforce memory limits

All allocations related to RZFP batch reading must be accounted for:

- output slice buffers
- two read windows
- compressed-window cache
- temporary decode arrays
- request/task metadata estimates

If a 2 GiB limit cannot hold all 100 outputs at once, split into deterministic batches. Never silently increase the user limit.

The benchmark CSV must report:

```text
memory_limit_mode
memory_limit_bytes
output_batch_size
window_cache_capacity_bytes
peak_accounted_bytes
```

## 6. Work package C: bounded compressed-window cache

### C1. Add cache implementation

Files:

- new: `include/erwt3d/window_cache.hpp`
- new: `src/window_cache.cpp`

Cache compressed RZFP payload windows, not full decompressed slices.

```cpp
struct WindowCacheKey {
    uint64_t file_identity = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
};

class BoundedWindowCache {
public:
    explicit BoundedWindowCache(uint64_t capacity_bytes = 0);

    bool get(const WindowCacheKey& key, const uint8_t*& data, uint64_t& size);
    void put(const WindowCacheKey& key, std::vector<uint8_t>&& data);
    void clear();

    uint64_t capacityBytes() const;
    uint64_t residentBytes() const;
    uint64_t hitCount() const;
    uint64_t missCount() const;
};
```

Use LRU eviction. Entries larger than capacity are read normally and are not inserted. Cache memory must never exceed the configured capacity.

### C2. Integrate with RZFP windowed reading

Files:

- `include/erwt3d/rzfp_reader.hpp`
- `src/rzfp_reader.cpp`

Before disk `pread`, check the user-space cache by file identity, offset and size. On miss, read into the normal window buffer and optionally move/copy the completed compressed window into the cache.

Do not cache Raw X auxiliary planes in this cache. Raw X already has direct contiguous access and should not consume the main RZFP window-cache budget.

Expose cache profile fields:

```text
window_cache_hits
window_cache_misses
window_cache_resident_bytes
window_cache_saved_read_bytes
```

### C3. Cache lifecycle by benchmark mode

Add benchmark protocol:

```cpp
enum class BenchmarkCacheMode {
    StableAuto,
    ColdRound,
    ColdGroup,
    Warm
};
```

Behavior:

- `StableAuto`: production behavior; do not clear the main payload or user-space cache between groups
- `ColdRound`: clear OS payload cache best-effort and clear user-space cache once before all six groups
- `ColdGroup`: clear OS payload cache best-effort and clear user-space cache before every group
- `Warm`: run an unmeasured warm-up round, retain OS and user-space cache, then run measured rounds

Normal library calls must not automatically discard the main RZFP payload after each read. Cache dropping is a benchmark-control operation.

## 7. Work package D: HDD read/write phase separation

Files:

- `tools/erwt3d_bench_rzfp.cpp`
- optional shared benchmark helper

On a single HDD, avoid interleaved input reads and output writes.

Preferred group flow:

1. pre-create output files;
2. allocate the largest batch permitted by the strict memory budget;
3. read and decode all slices in the batch;
4. finish input reading for that batch;
5. write outputs sequentially;
6. proceed to the next batch only when required by memory limits.

When memory permits, all 100 random outputs should be retained in memory and written after group reading completes.

I/O concurrency:

- one HDD input I/O thread
- decode threads default to `min(physical cores, 8)` initially; permit CLI override
- output writing occurs after the read phase, not concurrently with HDD input reads

Do not create dozens of HDD read threads merely because the CPU has many cores.

## 8. Work package E: benchmark modes and request ordering

Add CLI options:

```text
--benchmark-cache-mode stable-auto|cold-round|cold-group|warm
--rounds N
--memory-limit-mb auto|N
--window-cache-mb auto|N|0
```

Requests remain deterministic:

- fixed seed by default
- 100 random slices per axis
- 10 continuous slices per axis

If the program controls group order, use:

```text
X random
X continuous
Y random
Z random
Y continuous
Z continuous
```

This isolates Raw X groups and places Y/Z main-payload groups together. Also retain an option to reproduce the official/original order exactly.

For each mode and round output:

```text
device_sequential_mb_s
device_seek_ms
benchmark_cache_mode
memory_limit_bytes
window_cache_capacity_bytes
selected_strategy
strategy_reason
predicted strategy times
requested_bytes
actual_read_bytes
read_amplification
window cache hits/misses
io/decode/scatter/write/total time
request seed
round index
```

Report mean, median, minimum, maximum and coefficient of variation.

## 9. Work package F: CentOS 7 compatibility

P4 supports CentOS 7 only. Windows-specific work is deferred.

Requirements:

- use POSIX `pread`, `pwrite`, `posix_fadvise`, `posix_fallocate`, `fsync` and `/proc/meminfo`
- no correctness dependency on O_DIRECT
- do not compile the release binary with an unconditional `-march=native`
- provide a generic x86-64 build
- optional SIMD paths require runtime CPU feature detection
- document the required compiler toolset explicitly

Recommended documented build environment:

```text
CentOS 7
GCC 9 or newer from devtoolset
CMake version compatible with the project
x86-64 generic release build
```

If `std::filesystem` linking requires `-lstdc++fs` under the selected compiler, handle it in CMake or replace the benchmark-only filesystem use with POSIX directory creation.

## 10. Tests

### T1. Device calibration

- verify pre- and post-probe DONTNEED paths are invoked through a test hook
- verify small-file fallback remains 80 MB/s and 12 ms
- verify high-variance samples choose a conservative physical estimate

### T2. Pure adaptive strategy tests

Construct actual cost inputs and assert:

- 66 MB/s plus 21 GiB payload never selects FullPayloadScan
- 150 MB/s obeys the 120 s and 20% advantage rules
- 300 MB/s may select FullPayloadScan when it is genuinely at least 20% faster
- 5% gaps select lower-read-volume strategy
- 20% gaps select the predicted winner
- rejected Fullscan falls back to the faster of Whole and Selective

### T3. Pilot tests

- pilot recalculates each strategy I/O component separately
- strategy ordering can change when observed bandwidth changes
- 3 s and 256 MiB caps are respected
- slow-device absolute rejection cannot be bypassed by pilot

### T4. Memory budget tests

- explicit 2048 MiB is never exceeded
- explicit 8192 MiB produces a larger batch/cache than 2048 MiB
- auto uses no more than 50% MemAvailable and no more than 32 GiB
- explicit invalid or too-small values fail clearly
- allocations are batched rather than silently exceeding limits

### T5. Window-cache tests

- exact-key hit returns identical bytes
- capacity is never exceeded
- LRU eviction order is deterministic
- oversize entries are not cached
- `clear()` resets residency and lifecycle state
- cache hit avoids a physical read in an integration test

### T6. Cache-mode integration

Using a real small RZFP file:

- StableAuto preserves user-space cache between groups
- ColdRound clears once
- ColdGroup clears before every group
- Warm performs an unmeasured warm-up and then records measured rounds
- all modes produce numerically valid slices

### T7. Existing regressions

- all current CTest targets pass
- Raw X X-plane reads remain bit-exact
- RZFP error remains within the established tolerance
- no `.github/workflows` files are added or modified

## 11. Full-scale validation gates

### Gate 0: clean build

```bash
rm -rf build-p4
cmake -S . -B build-p4 -DCMAKE_BUILD_TYPE=Release -DERWT3D_ENABLE_RZFP=ON
cmake --build build-p4 -j16
ctest --test-dir build-p4 --output-on-failure -j8
```

### Gate 1: structural data

Use odd and boundary-crossing dimensions. Verify every strategy and cache mode produces equivalent output.

### Gate 2: 1 GiB proxy

Run simulated 66, 150 and 300 MB/s profiles. Verify Auto strategy is stable and profile output is complete.

### Gate 3: 20 GB HDD

Run `cold-group`, `cold-round`, `stable-auto` and `warm`, three measured rounds each.

Acceptance:

- X random <= 23 s
- composite <= 17 s in the existing validated environment
- same-mode CV <= 8%
- no correctness or storage regression

### Gate 4: 50 GB HDD

First run X, Y and Z random independently. Then run the complete six-group round.

Acceptance:

- X random <= 30 s
- on a device below 100 MB/s, Y/Z never select FullPayloadScan for a payload above 8 GiB
- Auto <= 1.15 times the best forced strategy on the same device and cache mode
- selected strategy is stable across three same-mode rounds
- reported actual bytes and cache hit statistics are internally consistent

The initial target for Y/Z is to reduce unnecessary disk bytes. Cold and warm wall times are not required to be identical, but no strategy may degrade catastrophically when the cache is cold.

## 12. Commit sequence

1. `fix: harden P3 device calibration and strategy decisions`
2. `feat: add strict and automatic memory budgeting`
3. `feat: add bounded RZFP compressed window cache`
4. `bench: add cold-round cold-group and warm protocols`
5. `perf: separate HDD read decode and output phases`
6. `test: cover HDD profiles memory budgets and cache lifecycle`
7. `docs: document CentOS 7 HDD competition workflow`

Do not combine all work into one unreviewable commit.

## 13. Final definition of done

P4 is complete when:

- P3 calibration cannot be inflated by already-cached probe ranges
- adaptive strategy behavior is tested using real decisions, not only configuration-value assertions
- 66/150/300 MB/s cost scenarios are covered
- strict 2 GiB mode works without hidden over-allocation
- auto mode uses available memory conservatively and reproducibly
- compressed-window cache obeys a hard capacity
- cold-group, cold-round and warm benchmark protocols exist
- normal production mode does not deliberately discard reusable main-payload cache
- HDD input reads and output writes are not unnecessarily interleaved
- the project builds in the documented CentOS 7 toolchain
- all existing and new tests pass locally
- 20 GB and 50 GB Raw X performance does not regress
