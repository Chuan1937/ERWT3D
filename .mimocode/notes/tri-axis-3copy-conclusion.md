# Tri-Axis ZFP 3-Copy 方案结论：不可行

## 分支说明

分支 `experiment/tri-axis-zfp-3copy` 保存了本次探索的完整代码、计划与实测数据。
包含：

- ZFP fixed-rate 扫描器 `tools/erwt3d_scan_zfp_rates.cpp`
- Tri-axis 新格式头 `include/erwt3d/tri_format.hpp`
- Tri-axis reader/writer `src/tri_reader.cpp`, `src/tri_writer.cpp`
- Tri-axis 工具链 `convert_tri`, `verify_tri`, `bench_tri`
- 原计划文档 `.mimocode/plans/1783437890341-swift-river.md`
- 后续 sidecar 重构计划 `.mimocode/plans/1783477972120-witty-river.md`

## 方案概述

为从根本上拉平 X/Y/Z 三轴切片读取时间，尝试为每个轴独立存储一份按该轴最优顺序排列的 block 数据：

- X 轴文件区：`bx` outer → `bz` → `by` → block
- Y 轴文件区：`by` outer → `bz` → `bx` → block
- Z 轴文件区：`bz` outer → `by` → `bx` → block

每个 block 使用 ZFP fixed-rate（rate=16 bpp，128 bytes/block）压缩，并对不满足 `rel_err < 1e-3` 的 block 保存原始 256 bytes 作为 exception。

## 20GB 实测数据

测试集：`/mnt/d/CUP/cup_3d_small.dat`（801 × 2405 × 2501，19.27 GB）

### 转换

```bash
./build/erwt3d_convert_tri --raw cup_3d_small.dat --output cup_3d_small_tri16.tri \
  --nx 801 --ny 2405 --nz 2501 --rate 16 --threads 8 --memory-limit-mb 4096
```

| 指标 | 数值 |
|------|------|
| 总 block 数 | 75,747,252 |
| exception block 数 | 7,887,086（10.4%） |
| 输出文件大小 | 32.3 GB |
| 存储比 | **1.677x** |
| 转换耗时 | **17m20s** |

### 正确性

```bash
./build/erwt3d_verify_tri --raw cup_3d_small.dat --tri cup_3d_small_tri16.tri \
  --nx 801 --ny 2405 --nz 2501 --samples 1000 --threads 8
```

结果：`checked=1000, failed=0, passed=true`。

### 三轴 benchmark（HDD，8 线程）

```bash
./build/erwt3d_bench_tri --input cup_3d_small_tri16.tri --output-dir bench_tri_20g \
  --threads 8 --memory-limit-mb 4096 --hdd
```

| 组别 | 时间（秒） | 读（秒） | 写（秒） |
|------|-----------|---------|---------|
| X random | **156.27** | 146.92 | 9.30 |
| Y random | 48.59 | 45.46 | 3.07 |
| Z random | 48.86 | 45.82 | 2.98 |
| X continuous | 16.93 | — | — |
| Y continuous | 5.61 | — | — |
| Z continuous | 4.80 | — | — |
| **T_composite** | **46.84** | — | — |

## 为什么这个方案不行

### 1. 存储比超过 1.5x 硬约束

rate=16 时三份轴数据已达 1.5x，加上 10.4% exception 后实际 **1.677x**，超出比赛 1.5x 上限。存储分只剩 18/20 且会继续扣。即使把 rate 降到 12：

- 三份轴数据 = 3 × 12/32 = 1.125x
- 但 P1 扫描显示 20GB 在 rate=12 时 exception 接近 100%
- 实际存储只会更高

因此 **3-copy 物理布局在 1.5x 约束下没有可行参数空间**。

### 2. X 轴天然不平衡

数据形状 nx=801 ≪ ny=2405 ≪ nz=2501 导致：

| 轴 | 每 slice 读取数据量 | random 读耗时 |
|----|-------------------|--------------|
| X | 46 MB | 156s |
| Y | 15 MB | 49s |
| Z | 15 MB | 49s |

X slice 的数据量是 Y/Z 的 3 倍，因为固定 `bx` 后还要遍历最大的两个维度。三轴独立布局只能保证“每次读一个连续 slab”，但无法消除数据形状导致的 slab 大小差异。

### 3. 性能不如现有 Morton 主线

当前主格式在同样 20GB / HDD / 4GB 内存条件下：

| 指标 | Morton 主线 | Tri-axis 3-copy |
|------|------------|-----------------|
| T_composite | **17.49s** | 46.84s |
| 存储比 | **0.932x** | 1.677x |

Tri-axis 在性能和存储上双双落后。

### 4. 转换时间不可接受

即使做了单遍 raw 读取、单次压缩、批量任务、大块 pwrite 优化后，20GB 转换仍需 17 分钟。50GB 数据集预计超过 45 分钟，且预处理时间虽不计分，但严重影响迭代效率。

## 教训

1. **不要为每个轴独立存一份 block 数据**。3 份数据是存储比超标的直接原因。
2. **ZFP fixed-rate 对近零值极其敏感**。20GB 有 ~11% 近零点，导致 10.4% block 被标记为 exception。
3. **HDD/9p 对小 pwrite 极不友好**。早期实现中每 slab 1.2 万次小 pwrite 让写阶段卡死；必须 gather 成大块再写。
4. **数据形状会吃掉理论优势**。即使三轴都变“顺序读”，slab 大小差异仍会造成轴间不平衡。

## 若仍想继续 tri-axis 方向

唯一可能的路径是 witty-river 计划中的 **单 Z 布局 + X/Y sidecar**：

- 只存一份 Z 布局 block 数据（rate=16 时 0.5x）
- X/Y 切片走预计算的 YZ/XZ sidecar
- 预期存储 ≈ 0.5x + sidecar_x + sidecar_y
- 20GB 的 YZ sidecar 已知可压到 ~0.49x，若 XZ sidecar 类似，则总存储 ≈ 1.48x，刚好压线

但该路径风险和复杂度都很高，且需要重新实现 reader/writer 的 sidecar 读写路径。

## 推荐决策

**放弃 tri-axis 3-copy ZFP 方案**。资源应优先投入：

- 主 Morton 格式的 X-band sidecar（已完成代码在同分支中）
- 或当前主格式的其他 I/O 优化

---

记录时间：2026-07-08
测试环境：WSL2 / D 盘 HDD / 8 线程 / 4GB 内存限制
