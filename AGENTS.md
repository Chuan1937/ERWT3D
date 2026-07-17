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

## 当前性能

G 盘 HDD，`--hdd` 模式，4GB 内存限制（布局修正后 Z-fastest）：

| 数据集 | 格式 | T_composite | 存储比 | 备注 |
|--------|------|------------|--------|------|
| 20GB | LZ4 | 35.89s | 0.432x | 全部 superblock 压缩，X/Y/Z 随机切片访问均衡 |
| 20GB | LZ4 + sidecar s1 | 27.19s | 0.526x | X random 12.7s（sidecar 加速），Y/Z 受页缓存影响 |
| **20GB** | **LZ4 + sidecar s2** | **25.37s** | **0.479x** | **最优方案**，sidecar 868MB，3轮均值 CV≈2%（最佳单轮 22.09s） |
| 20GB | RZFP | 51.26s | 0.607x | X random 需全文件扫描 (fullscan) |
| 50GB | RZFP | 99.06s | 0.421x | ZFP 压缩有效，50GB→21GB |
| 50GB | LZ4 | 138.41s | 1.044x | LZ4 自动跳过压缩（估算 0.915 > 0.90），52GB 未压缩 |

> 布局修正（Z-fastest）后重新生成所有文件并验证通过。
> LZ4 20GB 所有 19760 个 superblock 均成功压缩。
> RZFP 全量 fast-full 验证：20GB 4.8B 点 0 失败，50GB 13.2B 点 0 失败。
> Sidecar 采用 plane-major 单次读取 + 8 线程并行压缩生成。

### 推荐方案

| 数据集 | 推荐格式 | T_composite | 存储比 | 说明 |
|--------|----------|------------|--------|------|
| 20GB | LZ4 + sidecar s2 | 25.37s | 0.479x | 主文件 7.8GB + sidecar 868MB，3轮均值 CV≈2% |
| 50GB | RZFP | 99.06s | 0.421x | 50GB→21GB，无需 sidecar，单次代表性成绩 |

> **测试环境说明**：本批次在 G 盘 HDD 上测试，磁盘顺序读取带宽约 200–220 MB/s，
> 低于之前 D 盘环境（~300 MB/s），因此绝对时间偏高。
> 20GB LZ4+s2 T_composite 在 D 盘预期约 12–15s，50GB RZFP 预期约 60–70s。
> 相对排名（20GB LZ4 优于 RZFP，50GB RZFP 优于 LZ4）不受磁盘差异影响。
> LZ4 stride=2 在存储和性能之间取得最佳平衡。
> 3 轮重复测试：LZ4+s2 T_composite 均值 25.37s，CV ~2%（首轮 22.09s 为 sidecar 生成后热缓存）。

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
