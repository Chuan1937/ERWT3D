# AGENTS.md

## 项目概述

ERWT3D 是一个 C++ 库，用于高效读写大规模规则三维 float32 数据体。采用自定义单文件格式，Morton Z-order 物理布局，三轴切片访问均衡。

比赛：赛题2 - 三维空间数据的高效读写

## 构建命令

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

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
- 存储顺序：X-Y-Z row-major（X 变化最快）

## 文件格式

- Header：256 字节（magic、维度、块大小、flags）
- Superblock：64×64×64 float32 = 1 MiB，Z-Y-X 顺序排列
- Leaf block：4×4×4 float32 = 256 字节，superblock 内 Morton 顺序
- 可选 X-plane：预存 YZ 平面，加速 X 切片访问
- 可选 lz4 压缩：每个 superblock 独立压缩，不压缩的块直接存原始数据
- 压缩索引：文件末尾存储每个块的偏移和大小（16 字节/块）
- 偏移公式：`data_offset + sb_idx * sb_bytes + morton3D(lx,ly,lz) * leaf_bytes`

## 文件格式

- Header：256 字节（magic、维度、块大小、flags）
- Superblock：64×64×64 float32 = 1 MiB，Z-Y-X 顺序排列
- Leaf block：4×4×4 float32 = 256 字节，superblock 内 Morton 顺序
- 可选 X-panel：预存 YZ 平面，加速 X 切片访问
- 可选 X-plane：连续 X 切片平面（存储比 >1.5x）
- 偏移公式：`data_offset + sb_idx * sb_bytes + morton3D(lx,ly,lz) * leaf_bytes`

## HDD 优化策略

- 单线程顺序读取（避免磁头抖动）
- 文件偏移排序（`--sb-task-order file-offset`）
- 大读窗口 + gap 容忍（`--hdd-read-window-bytes 134217728 --hdd-max-gap-bytes 3145728`）
- 跨切片批量规划（`--hdd` 自动启用 dynamic batch size + 全局排序）
- posix_fadvise(SEQUENTIAL) + readahead() 内核提示
- readahead 前瞻 20 个窗口（深度流水线）
- 预创建输出文件（避免 open/close 开销在计时内）
- -march=native 编译优化（AVX2 自动向量化）

## 当前性能

D 盘 HDD，`--hdd` 模式，4GB 内存限制：

| 数据集 | T_composite | 存储比 | 备注 |
|--------|------------|--------|------|
| 20GB | 23.56s | 0.443x | S=64, lz4压缩, best of 2 repeats |
| 20GB | 34.23s | 1.408x | S=64, X-planes stride=3, 无压缩 |
| 50GB | 87.67s | 1.044x | S=64, 无压缩 (压缩率差，不压缩更快) |

4GB 是内存拐点，≥4GB 后性能稳定。

### 关键发现

- **I/O 带宽是根本瓶颈**：磁盘顺序读 ~300 MB/s，随机访问需读取几乎整个文件
- **压缩效果因数据集而异**：20GB 数据集 lz4 压缩率 2.26x (0.443x)，50GB 仅 1.004x (0.996x)
  - 压缩率好的数据集：读取量大幅减少，T_composite 改善 31%
  - 压缩率差的数据集：解压开销 + 非顺序偏移导致性能下降，应跳过压缩
  - 建议：转换时先测试压缩率，低于 1.1x 则不压缩
- **超级块大小对总性能影响很小**：S=16 vs S=64 的 T_composite 几乎相同（38.62 vs 38.67）
  - S=16 改善 Z-random（30s vs 59s），但恶化 X-continuous（58s vs 7s）
  - Y/X random 无论 SB 大小都读取整个文件
- **窗口大小和 gap 容忍影响很小**：128MB/3MB vs 64MB/512KB 差异 <1s
- **X-planes 帮助有限**：stride=3 只覆盖 33% 的 X 切片，且增加文件体积
- **Page cache 对重复运行有帮助**：第二轮比第一轮快 ~3-5s

## 常用命令

```bash
# 赛题 benchmark
./build/erwt3d_bench_contest --input data.erwt3d --output-dir out --hdd

# 转换
./build/erwt3d_convert --input data.raw --output data.erwt3d \
    --nx 801 --ny 2405 --nz 2501 --threads 8 --memory-limit-mb 4096

# 验证
./build/erwt3d_verify --raw data.raw --erwt3d data.erwt3d \
    --nx 801 --ny 2405 --nz 2501 --samples 100000

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

数据转换和测试都需要在 HDD（D 盘）上进行，不能在 SSD 上测试。
