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
| `erwt3d_convert` | **统一自动转换**（Raw↔LZ4+embedded XP / RZFP） |
| `erwt3d_contest` | **统一比赛切片**（自动识别LZ4/RZFP，330个.dat） |
| `erwt3d_verify` | 正确性验证 |
| `erwt3d_info` | 文件信息查看 |
| ~~`erwt3d_bench_contest`~~ | Historical / superseded — 请用 `erwt3d_contest` |
| ~~`erwt3d_precompute_x`~~ | Historical / superseded — XP 已内嵌 |
| `erwt3d_convert_rzfp` | Raw ↔ RZFP 格式转换 |
| `erwt3d_verify_rzfp` | RZFP 正确性验证 |
| `erwt3d_bench_rzfp` | RZFP 基准测试 |
| `erwt3d_rzfp_xplane_gen` | 生成 RZFP 2D X-plane sidecar |

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

# 文件格式

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

## 当前性能（main, HEAD `6383261`）

### SSD Guest-cold（WSL2 ext4, i9-10850K 8C/16T）

| 数据 | 格式 | 路径 | Threads | process_e2e | T_composite | RSS |
|------|------|------|:-------:|:-----------:|:-----------:|:---:|
| 50GB | RZFP axis-leaf | standard reader | 16 | 22.6s | 3.8s | 8.5GB |
| 50GB | RZFP axis-leaf | **cold executor**¹ | **16** | **17.7s** | **3.0s** | **8.9GB** |
| 20GB | LZ4 unified | standard reader | 16 | 8.9s | 1.4s | 3.4GB |

¹ 使用 `--ssd-cold-backend pread` 显式启用。构建 `-O3 -march=x86-64-v3`。

### HDD Guest-cold（G: 盘 via 9p, 同机）

| 数据 | 格式 | 路径 | Threads | process_e2e |
|------|------|------|:-------:|:-----------:|
| 50GB | RZFP axis-leaf (external .xal) | standard reader | 16 | 85.8s |
| 20GB | LZ4 unified | standard reader | 16 | 69.3s |

### 比赛评分参考

```
T_composite = (T_xr + T_yr + T_zr + T_xc + T_yc + T_zc) / 6
性能得分 = (基准时间 / T_composite) × 60
存储得分：≤1.5x → 20分
```

### 关键验证

- 39/39 CTest 通过
- 330/330 SHA256 一致
- RZFP violations=0，max_relative_error<1e-3

## 推荐参赛命令

### 50GB RZFP（推荐 cold executor，需硬件支持）

```bash
# 显式使用 cold executor
./build/erwt3d_contest --input data.erwt3d --output-dir OUT \
  --positions-file positions_big.csv --threads 16 \
  --io-profile ssd --ssd-cold-backend pread

# 或自动路径（标准 reader）
./build/erwt3d_contest --input data.erwt3d --output-dir OUT \
  --positions-file positions_big.csv --threads 16 \
  --io-profile auto
```

### 20GB LZ4

```bash
./build/erwt3d_contest --input data.erwt3d --output-dir OUT \
  --positions-file positions_small.csv --threads 16 \
  --io-profile auto
```

## RZFP Axis Leaf（PR #59 最终方案）

RZFP 专用三轴 leaf 副本格式：将原始 4×4×4 leaf payload 按 axis 重新排列为三个独立文件（`.xal/.yal/.zal`），每条记录保留原 descriptor ID。

```bash
# 转换
./build/erwt3d_rzfp_axis_repack \
  --input data.rzfp --output data_axis.rzfp \
  --memory-limit-mb 32768

# 产出: data_axis.rzfp + .xal + .yal + .zal

# 读取
./build/erwt3d_contest --input data_axis.rzfp \
  --threads 8 --memory-limit-mb 4096 --io-profile hdd
```

### 50GB 最终性能

| 指标 | 原 RZFP merged | Axis leaf | 提速 |
|------|:---:|:---:|:---:|
| process_e2e | **119s** | **20.7s** | **5.7×** |
| T_composite | 19.8s | 3.5s | 5.7× |
| merged_read | 117s | 19.1s | 6.1× |
| total_write | 2.3s | 1.6s | — |
| peak RSS | 24.0 GB | 8.5 GB | -65% |
| 存储比 | 0.42× | **1.30× ≤ 1.50× ✓** | — |
| SHA256 vs legacy | — | **330/330 MATCH ✓** | — |
| 冷缓存 CV (3x) | — | **2%** | — |

### 20GB LZ4 YZ whole-plane

| 指标 | 值 |
|------|:---:|
| process_e2e | **7.6s** |
| 存储比 | **0.99×** |
| SHA256 vs legacy | **330/330 MATCH ✓** |

### 推荐参赛命令

```bash
# 统一格式（单文件 .erwt3d）:
./build/erwt3d_convert --input raw.dat --output data.erwt3d \
  --nx N --ny N --nz N --threads auto --memory-limit-mb auto

# 20GB LZ4（SSD）:
./build/erwt3d contest --input data.erwt3d --output-dir OUT \
  --threads 8 --memory-limit-mb 4096 --io-profile auto

# 50GB RZFP axis leaf（SSD）:
./build/erwt3d contest --input data.erwt3d --output-dir OUT \
  --threads 8 --memory-limit-mb 4096 --io-profile auto
```

- **threads=8**：i9-10850K 8 物理核，16 线程 SMT 竞争减速
- **memory=4GB**：RZFP axis leaf 约 8.5GB peak RSS，4GB 限制足够
- **--io-profile auto**：自动检测 embedded layout，LZ4+YZ/RZFP+axis 强制走 HDD 大窗口

### 统一单文件格式（`main`, HEAD `26c422d`）

| 数据集 | 内部格式 | 存储比 | cold R1 | cold R2 | cold R3 | 中位 |
|------|---------|:---:|:--:|:--:|:--:|:--:|
| 20GB LZ4 | LZ4 + YZ whole-plane | 0.92× | 12.0s | 7.2s | **5.9s** | **7.2s** |
| 50GB RZFP | RZFP + XYZ axis-leaf | 1.30× | 21.2s | 24.4s | 25.3s | **24.4s** |

> WSL2 VHDX 的 `drop_caches` 不能清除 Windows host 缓存，50GB 冷缓存递增是已知局限。
> 50GB warm 之前测过 21.1s（`53e14b0`）。SHA256 与旧多文件 330/330 一致 ✓。

### 推荐方案

| 目标 | 20GB | 50GB |
|------|------|------|
| **正式参赛** | LZ4 + YZ whole-plane (~7s, 0.92×) | RZFP + XYZ axis-leaf (~21s, 1.30×) |
| **方法** | 单 plane 单 LZ4 record，直接输出 | 三轴 leaf 副本，轴向 slab 读 |
| **正确性** | SHA256 MATCH ✓ | SHA256 MATCH ✓ |

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
