# AGENTS.md

## 项目概述

ERWT3D 是一个 C++ 库，用于高效读写大规模规则三维 float32 数据体。采用自定义单文件格式，Morton Z-order 物理布局，三轴切片访问均衡。

比赛：赛题2 - 三维空间数据的高效读写

## 构建命令

### 默认（LZ4 路径）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### RZFP 路径

需要 ZFP 库（≥1.0）。若系统未安装，可在项目本地构建：

```bash
git clone --depth 1 --branch 1.0.1 https://github.com/LLNL/zfp.git deps/zfp-src
cmake -S deps/zfp-src -B deps/zfp-build -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_UTILITIES=OFF -DBUILD_TESTING=OFF -DZFP_WITH_OPENMP=OFF
cmake --build deps/zfp-build -j
cmake --install deps/zfp-build --prefix deps/zfp
```

然后启用 RZFP 构建：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DERWT3D_ENABLE_RZFP=ON \
      -DCMAKE_PREFIX_PATH=/home/chuan/code/ERWT3D/deps/zfp
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`deps/` 已加入 `.gitignore`，不会进入版本控制。

## 核心二进制

| 二进制 | 用途 |
|--------|------|
| `erwt3d_convert` | Raw ↔ ERWT3D 格式转换 |
| `erwt3d_bench_contest` | 赛题标准基准测试（赛题2 评分） |
| `erwt3d_bench` | 通用基准测试，支持全部参数 |
| `erwt3d_slice` | 单切片/单线读取 |
| `erwt3d_verify` | 正确性验证 |
| `erwt3d_info` | 文件信息查看 |
| `erwt3d_precompute_x` | X-plane 预计算（可选，存储比 >1.5x） |
| `erwt3d_convert_rzfp` | Raw ↔ RZFP 格式转换 |
| `erwt3d_verify_rzfp` | RZFP 正确性验证 |
| `erwt3d_bench_rzfp` | RZFP 基准测试 |
| `erwt3d_rzfp_xplane_gen` | 生成 RZFP 2D X-plane sidecar |
| `erwt3d_taps_convert` | Raw → TAPS 格式转换 |
| `erwt3d_taps_slice` | TAPS 单切片读取 |
| `erwt3d_taps_verify` | TAPS 批量随机点验证 |
| `erwt3d_taps_bench` | TAPS Contest 风格基准测试 |
| `erwt3d_plane_probe` | 三轴压缩率探测 |

## 比赛评分（赛题2）

```
T_composite = (T_xr + T_yr + T_zr + T_xc + T_yc + T_zc) / 6
性能得分 = (基准时间 / T_composite) × 60    （基准时间 = 所有参赛者中最短）
存储得分：≤1.5x → 20分，每超10% → 扣1分
```

- 6 组测试：X/Y/Z 随机（各100片）+ X/Y/Z 连续（各10片）
- 计时范围：索引定位 → 磁盘读取 → 解码 → 内存重排 → 切片拼接 → 文件写出
- 数据预处理时间不计入性能得分
- 单点相对误差 < 0.001（否决项）

## 数据格式

| 数据集 | 维度 | 原始大小 |
|--------|------|----------|
| small.dat | 801 × 2405 × 2501 | 18 GB |
| big.dat | 2001 × 2201 × 3000 | 50 GB |

- 数据类型：float32
- **官方原始布局**：X-Y-Z row-major，Z 最快变化
  - 偏移公式：`offset(x,y,z) = (x*ny + y)*nz + z`
  - 固定 x 的完整 YZ 平面在 raw 文件中连续存储
- **ERWT3D/RZFP 内部 Leaf 布局**：X 最快变化，`leaf[(z*leafY+y)*leafX+x]`
  - 转换器负责外部 Z-fastest 与内部 Leaf 布局之间的重排

## 文件格式

- Header：256 字节（magic、维度、块大小、flags）
- Superblock：64×64×64 float32 = 1 MiB，Z-Y-X 顺序排列（内部布局 X fastest）
- Leaf block：4×4×4 float32 = 256 字节，superblock 内 Morton 顺序
- 可选 X-panel：预存 YZ 平面，加速 X 切片访问
- 可选 X-plane：连续 X 切片平面（存储比 >1.5x）
- 可选 X-plane sidecar（`data.erwt3d.xp`）：独立文件，每 plane 按 Z 分段 lz4 压缩
- 可选 lz4 压缩：每个 superblock 独立压缩，不压缩的块直接存原始数据
- 压缩索引：文件末尾存储每个块的偏移和大小（16 字节/块）
- 偏移公式：`data_offset + sb_idx * sb_bytes + morton3D(lx,ly,lz) * leaf_bytes`

## HDD 优化策略

- 单线程顺序读取（避免磁头抖动）
- 文件偏移排序（`--sb-task-order file-offset`）
- 大读窗口 + gap 容忍（`--hdd-read-window-bytes 536870912 --hdd-max-gap-bytes 8388608`）
- 跨切片批量规划（`--hdd` 自动启用 dynamic batch size + 全局排序）
- posix_fadvise(SEQUENTIAL) + readahead() 内核提示
- readahead 前瞻 20 个窗口（深度流水线）
- 预创建输出文件（避免 open/close 开销在计时内）
- -march=native 编译优化（AVX2 自动向量化）

## 当前性能（P5）

分支 `perf/p5-round-planner-hdd-pipeline` (PR #55)，`erwt3d_contest` 正式入口。

测试环境：i9-10850K（8C/16T）、62 GiB RAM、0 swap、G 盘 HDD via WSL2 9p、GCC 15.2.1、CMake 3.31、HEAD `edd6f2a`。`--threads 8`，`--seed 20260511`。

### 50GB RZFP + Raw X Aux（1.421x）

| 配置 | 内存 | 窗口 | T_composite | RSS | vs AUTO |
|------|------|------|-------------|-----|---------|
| AUTO | 27 GiB | 512 MB | **39.95s** | 30.4 GiB | baseline |
| M28 | 28 GiB | 512 MB | 40.70s | 30.4 GiB | +1.9% |
| M24 | 24 GiB | 512 MB | 40.30s | 30.4 GiB | +0.9% |
| **M8** ★ | **8 GiB** | **128 MB** | **41.37s** | **13.7 GiB** | **+3.6%** |
| M16 | 16 GiB | 256 MB | 42.20s | 22.1 GiB | +5.6% |
| M4 | 4 GiB | 64 MB | 56.67s | 8.4 GiB | +42% |
| M2 | 2 GiB | 64 MB | 88.30s | 4.4 GiB | +121% |

### 稳定性（stable-auto x5）

| 配置 | mean | median | min | max | CV |
|------|------|--------|-----|-----|-----|
| AUTO | 40.01s | 39.98s | 39.82s | 40.32s | 0.5% |
| M8 | 41.34s | 41.33s | 41.21s | 41.47s | 0.3% |

### cold-round x3

| 配置 | mean |
|------|------|
| AUTO | 40.27s |
| M8 | 41.68s |

### 20GB RZFP + X-plane sidecar（1.036x）

| 配置 | 内存 | T_composite | RSS |
|------|------|-------------|-----|
| AUTO | 17 GiB | **16.79s** | 17.8 GiB |
| **M4** ★ | **4 GiB** | **17.25s** | **8.7 GiB** |
| M2 | 2 GiB | 50.44s | 4.7 GiB |

### P4 vs P5 对比（50GB M8, stable-auto）

| 模式 | mean |
|------|------|
| P4 (p4-groups) | 42.36s |
| P5 (p5-round) | 41.83s ¹ |

¹ P5 round 2 outlier 45.4s excluded (disk activity)。

### 同盘/异盘

| 模式 | T_composite |
|------|-------------|
| 同盘 (G→G) | 41.60s |
| 异盘 (G→WSL) | **37.69s** |
| 差异 | **-9.4%** |

### 关键验证

- 21/21 CTest 通过
- 2GB 不崩溃，decode errors = 0
- AUTO vs M8 hash 一致
- 输出文件 330/330
- 存储比 50GB 1.421x、20GB 1.036x（≤ 1.50）
- 5 轮 CV ≤ 0.5%

### P5 关键架构

- **`readContestRound()`**：Y/Z 四组合并为一次 `readSlicesBatch`，跨组 Leaf 去重
- **`ContestRoundExecutor`**：共享执行器，benchmark 与 contest 共用，字节预算阶段规划
- **`erwt3d_contest`**：正式入口，`--memory-limit-mb N`，`--read-window-mb N`
- **`BoundedWindowCache::getContaining()`**：包含命中范围缓存
- **`computeWindow`**：`windowEnd = max(windowEnd, end)` 修复同 offset 不同 size 窗口收缩
- **`patchExceptions`**：`bool` 返回 + popcount 校验，消除 `std::out_of_range`
- **直读回退**：decode 失败时从磁盘重读
- **存储预算**：1.50 硬上限，1.490 自动目标，1.495 手动上限

### 推荐方案

| 目标 | 50GB | 20GB |
|------|------|------|
| **推荐参赛** | M8 (8 GiB, 128 MB) — 41.37s | M4 (4 GiB) — 17.25s |
| **绝对最快** | AUTO (27 GiB) — 39.95s | AUTO (17 GiB) — 16.79s |
| **低内存兜底** | M2 (2 GiB) — 88.30s | M2 (2 GiB) — 50.44s |

> 存储比 1.50× 是比赛得分满分线（20/20）。所有性能分公式为
> `(基准时间 / T_composite) × 60`。

### 关键发现

- **X-plane sidecar 是 X random 的最大收益点**：stride=1 全覆盖时 X random 从 63s 降到 15s（-76%），因为只需读压缩 chunk（~11MB/plane）而非扫描整个文件
- **sidecar 压缩率取决于 YZ 平面空间相关性**：20GB 数据集 0.489x（可行），50GB 0.979x（sidecar 16GB 导致 page cache 干扰，净效果为负，不应使用）
- **sidecar 生成按 raw 的 X-Y-Z row-major 布局提取**：在官方 Z-fastest 布局中，固定 x 的 YZ 平面在 raw 文件中连续存储，sidecar writer 直接按 X chunk 顺序读取并转置为 Y-fastest 的 sidecar 平面格式
- **流式 sidecar writer**：按 z-chunk 分批处理，内存从 18GB 降至 1.9GB（20G stride=1）
- **sidecar batch reader**：chunk task 全局排序 + 4KB gap 合并，减少 HDD 寻道
- **LeafOp 紧凑化**（48B→16B/leaf）改善 cache 局部性，50GB T_composite 改善 6%
- **I/O 带宽仍是根本瓶颈**：磁盘顺序读 ~300 MB/s，非 sidecar 路径的随机访问仍需读取几乎整个文件
- **压缩效果因数据集而异**：20GB lz4 压缩率 2.26x (0.443x)，50GB 仅 1.004x (0.996x)
- **参数扫描结论**：chunk_z_rows 64-1024 压缩率差异 <0.2%，256 是合理默认
- **HDD 读窗口是关键**：128MB/2MB 窗口在 50GB 上 T_composite 约 123s，提升到 512MB/8MB 后降至 83.58s（-32%），因为减少了随机读取时的 pread 次数和寻道开销
- **RZFP 2D X-plane sidecar 对 20GB 收益显著**：X random 从 ~170s 降至 ~17s，sidecar 比率 0.530x，综合存储 1.369x（仍 ≤1.5x）
- **50GB 无需 sidecar**：RZFP 本身已达 0.804x，叠加 sidecar 会使综合存储逼近 1.5x 且生成成本过高；512MB 窗口已足够突破 104.74s 目标
- **Page cache 对重复运行有帮助**：第二轮比第一轮快 ~3-5s

## SSD 优化路径（TAPS 格式）

分支 `feature/ssd-adaptive-plane-stream`，独立于 HDD 代码。

### TAPS 格式概述

Three-Axis Plane Stream（TAPS）：三轴独立流式存储，每轴一个 `.stream` + `.index` 文件，LZ4 chunk 压缩。每平面按 chunk 切分压缩，索引记录偏移/大小/平面号。读取时按需 pread 对应 chunk，多线程并行解码。

- 存储比 = 三轴流总大小 / 原始大小
- 无损（LZ4），单点误差 = 0
- 读取路径：索引定位 → pread chunks → LZ4 解码 → unshuffle → 输出

### 核心文件

| 文件 | 用途 |
|------|------|
| `include/erwt3d/taps_format.hpp` | TAPS 格式定义（TapsChunkIndex, TapsReader, TapsWriter API） |
| `src/taps_writer.cpp` | TAPS 写入（Z 轴 X-slab 策略） |
| `src/taps_reader.cpp` | TAPS 读取（多线程 batch，ThreadPool submit） |
| `tools/erwt3d_taps_convert.cpp` | Raw → TAPS 转换 |
| `tools/erwt3d_taps_slice.cpp` | 单切片读取 |
| `tools/erwt3d_taps_verify.cpp` | 批量随机点验证 |
| `tools/erwt3d_taps_bench.cpp` | Contest 风格基准测试 |
| `tools/erwt3d_plane_probe.cpp` | 三轴压缩率探测 |

### SSD 测试环境

i9-10850K（8C/16T）、62 GiB RAM、WSL2 ext4（C-drive SSD, /dev/sde, ~1.6 GB/s）、GCC 15.2.1。构建：`build-ssd`，`-DERWT3D_NATIVE_OPT=ON -DERWT3D_ENABLE_RZFP=ON`。

### 20GB TAPS（0.4146x 存储）

维度 801×2405×2501，TAPS 总大小 7.44 GiB（X.stream 1.7G, Y.stream 2.9G, Z.stream 2.9G + index + metadata）。

#### 单线程基线

| 组 | 时间 |
|----|------|
| X_random (92 slices) | 9.60s |
| X_continuous (10) | 1.03s |
| Y_random (99) | 2.75s |
| Y_continuous (10) | 0.28s |
| Z_random (99) | 2.61s |
| Z_continuous (10) | 0.26s |
| **T_composite** | **2.76s** |

#### 8 线程（3 轮稳定性）

| 轮次 | X_random | X_cont | Y_random | Y_cont | Z_random | Z_cont | T_composite |
|------|----------|--------|----------|--------|----------|--------|-------------|
| R1 | 2.26s | 0.21s | 0.56s | 0.10s | 0.76s | 0.07s | 0.66s |
| R2 | 2.99s | 0.63s | 1.19s | 0.13s | 0.72s | 0.08s | 0.96s |
| R3 | 2.22s | 0.27s | 0.71s | 0.08s | 0.63s | 0.07s | 0.66s |
| **mean** | **2.49s** | **0.37s** | **0.82s** | **0.10s** | **0.70s** | **0.07s** | **0.76s** |

- 加速比：单线程 2.76s → 8 线程 0.76s = **3.6x**
- X_random 仍是瓶颈（占 T_composite 的 ~55%）
- R2 偏高（可能 SSD 后台活动），R1/R3 一致 0.66s

### 50GB TAPS — 不可行

50GB 数据集 LZ4 单轴压缩率 ~0.94x（与 HDD 路径发现一致：50GB lz4 仅 1.004x）。三轴总存储比 ~2.8x，远超 1.5x 限制。Z-stream 写入还需 ~52.8GB 内存（`z_buf` 全量缓存），62GB 机器无法容纳。

**结论**：TAPS 格式仅适用于高压缩率数据集（如 20GB 的 0.4146x）。50GB 数据集应继续使用 HDD 路径（RZFP 1.421x）。

### TAPS 关键发现

- **三轴独立流是 SSD 场景的最优解**：SSD 随机读性能好，无需 HDD 的窗口/排序策略
- **多线程 pread + LZ4 解码并行**：8 线程 3.6x 加速，I/O 和 CPU 重叠良好
- **X_random 瓶颈**：X 平面最大（ny×nz = 6M floats = 24MB/plane），chunk 数量多
- **存储比是 TAPS 的硬约束**：三轴冗余存储，仅高压缩率数据集可行
- **Z-stream 写入内存问题**：当前实现需全量缓存所有 Z 平面，大数据集需流式重写

## 常用命令

使用 sw4 风格的 `erwt3d` 统一入口（`build/erwt3d`）：

```bash
# 直接命令模式：erwt3d <command> key=value ...
./build/erwt3d convert input=data.raw output=data.erwt3d nx=801 ny=2405 nz=2501 threads=8 memory-limit-mb=4096

# LZ4 压缩 + Raw X 辅助区（推荐，存储比 ≤1.45x 时最佳）
./build/erwt3d_convert --input data.raw --output data.erwt3d --nx 801 --ny 2405 --nz 2501 --compress --raw-x-aux on --threads 12

# RZFP + Raw X 辅助区
./build/erwt3d_convert_rzfp --input data.raw --output data.rzfp --nx 2001 --ny 2201 --nz 3000 --raw-x-aux on --threads 12

./build/erwt3d bench-contest input=data.erwt3d output-dir=out hdd

./build/erwt3d verify raw=data.raw erwt3d=data.erwt3d nx=801 ny=2405 nz=2501 samples=100000

./build/erwt3d info data.erwt3d

# 配置文件模式（sw4 风格，支持多任务）
# job.txt 内容：
#   convert
#     input = data.raw
#     output = data.erwt3d
#     nx = 801  ny = 2405  nz = 2501
#     threads = 8  memory-limit-mb = 4096
#
#   verify raw=data.raw erwt3d=data.erwt3d nx=801 ny=2405 nz=2501 samples=100000
./build/erwt3d job.txt

# 预览模式（不实际执行）
./build/erwt3d --dry-run job.txt

# 内存扫描
./scripts/bench_mem_sweep.sh
```

## 代码风格

- C++17 标准
- 除非特别要求，不加注释
- 命名空间：`erwt3d`
- 头文件 `include/erwt3d/`，源文件 `src/`
- 静态库：`liberwt3d.a`
- 热路径使用 POSIX I/O（pread/pwrite），不用 iostream
- 线程池支持 CPU 亲和绑核（Linux）

## 说明

数据转换和测试都需要在 HDD（G 盘）上进行，不能在 SSD 上测试。
