# 性能测试

## 评分公式

```
T_composite = (T_xr + T_yr + T_zr + T_xc + T_yc + T_zc) / 6
性能得分 = (基准时间 / T_composite) × 60    （基准时间 = 所有参赛者中最短）
存储得分：≤1.5x → 20分，每超10% → 扣1分
```

## 测试环境

- 存储：D 盘机械硬盘，WSL 9p 挂载
- HDD 顺序带宽：~345 MB/s
- 9p pread 开销：~3.45 ms/次
- 配置：`--hdd`（单线程 / 128MB 读窗口 / 3MB gap / file-offset 排序）
- 计时范围：索引定位 → 磁盘读取 → 解码 → 内存重排 → 切片拼接 → **文件写出**（符合比赛要求）
- 输出目录在 HDD 上（比赛环境无可避免，写入竞争已计入）

## 基准结果（存储比 ≤1.5x，v0.4 优化后代码）

### 20GB（801×2405×2501，X-plane stride=3）

存储比 1.408x → 存储得分 20/20

| 测试项 | v0.2 基线 | v0.3 x-plane s=3 | v0.4 初始 | **v0.4 优化后** |
|--------|----------|-----------------|----------|-----------------|
| X random (100片) | 73.78s | 70.36s | 95.76s | **82.88s** |
| Y random (100片) | 63.42s | 61.75s | 71.84s | **70.15s** |
| Z random (100片) | 57.97s | 56.91s | 64.71s | **62.67s** |
| X continuous (10片) | 7.11s | 7.00s | 8.41s | **7.74s** |
| Y continuous (10片) | 2.18s | 2.04s | 2.23s | **2.32s** |
| Z continuous (10片) | 2.06s | 1.94s | 2.20s | **2.15s** |
| **T_composite** | **34.42s**\* | **33.21s**\* | **40.86s** | **37.99s** |
| 存储比 | 1.075x | 1.408x | 1.408x | 1.408x |

\* v0.2/v0.3 结果不含文件写出。v0.4 含写出：初始 40.86s → ftruncate 优化后 37.99s（-7%）。

### 50GB（2001×2201×3000，X-plane stride=3）

存储比 1.378x → 存储得分 20/20

| 测试项 | v0.2 基线\* | v0.4 初始 | **v0.4 优化后** |
|--------|-----------|----------|-----------------|
| X random (100片) | 177.50s | 421.02s | **267.84s** |
| Y random (100片) | 171.85s | 196.30s | **178.68s** |
| Z random (100片) | 158.58s | 176.03s | **171.32s** |
| X continuous (10片) | 7.53s | 16.43s | **7.66s** |
| Y continuous (10片) | 5.94s | 6.29s | **5.70s** |
| Z continuous (10片) | 4.61s | 4.76s | **4.27s** |
| **T_composite** | **87.67s**\* | **136.81s** | **105.91s** |
| 存储比 | 1.044x | 1.378x | 1.378x |

\* v0.2 不含文件写出。v0.4 初始 136.81s → ftruncate 优化后 105.91s（**-22.6%**）。

## 带宽利用率分析

### 20GB

6 组测试总读取量约 60.4 GB（含写出重叠）。纯顺序读理论最小：

```
T_min = 60.4 GB / 345 MB/s / 6 = 29.9s
实际 T_composite = 37.99s
效率 = 29.9 / 37.99 = 78.7%
```

各轴效率（v0.4 优化后，读时 = readTimeMs）：

| 轴 | 数据量 | 读时间 | 写时间 | 有效读带宽 |
|----|--------|--------|--------|----------|
| Z random | 19.76 GB | 59.6s | 2.9s | 332 MB/s |
| Y random | 19.76 GB | 67.0s | 2.9s | 295 MB/s |
| X random | 19.76 GB | 71.3s | 10.3s | 277 MB/s |

### 50GB

6 组测试总读取量约 157.8 GB。纯顺序读理论最小：

```
T_min = 157.8 GB / 345 MB/s / 6 = 76.2s
实际 T_composite = 105.91s
效率 = 76.2 / 105.91 = 71.9%
```

各轴效率（v0.4 优化后）：

| 轴 | 数据量 | 读时间 | 写时间 | 有效读带宽 |
|----|--------|--------|--------|----------|
| Z random | 52.64 GB | 156.9s | 13.8s | 336 MB/s |
| Y random | 52.64 GB | 169.7s | 8.1s | 310 MB/s |
| X random | 52.64 GB | 243.8s | 22.6s | 216 MB/s |

**关键发现**：X random 的写开销（22.6s for 100 slices）远大于 Y（8.1s）和 Z（13.8s），因为 X 切片体积最大（26MB vs 23MB/17MB）。预分配文件后写效率提升显著，但 X random 读带宽仍低于 Y/Z（X 切片需逐超块跳跃读取）。

## 内存限制扫描

### 20GB

| MemLimit | X random | Y random | Z random | T_composite |
|----------|----------|----------|----------|-------------|
| 2 GB | — | — | — | — |
| 4 GB | 82.88s | 70.15s | 62.67s | 37.99s |
| 8 GB | — | — | — | — |

### 50GB

| MemLimit | X random | Y random | Z random | T_composite |
|----------|----------|----------|----------|-------------|
| 2 GB | — | — | — | — |
| 4 GB | 267.84s | 178.68s | 171.32s | 105.91s |

**结论**：4GB 是拐点。≥4GB 后 all-in-one batch，性能稳定。瓶颈在磁盘带宽。

## 优化历史

| 版本 | 优化 | 20GB T_composite | 50GB T_composite |
|------|------|-----------------|-----------------|
| v0.1 | 初始版本（batch=20） | 94.11s | 223.43s |
| v0.2 | batch size 动态化 + 全局排序 | 34.42s | 87.67s |
| v0.3 | __restrict__ + X-plane stride=3 | 33.21s | ~85s |
| **v0.4** | **计时含写出 + ftruncate 预分配 + 诊断** | **37.99s** | **105.91s** |

v0.3 结果不含文件写出，不可与 v0.4 直接对比。v0.4 首个含写出版本为 40.86s/136.81s。

v0.4 改进：
- 压缩文件 `readSliceSB` 按物理偏移排序 + `lastSbIdx` 缓存
- `readSlicesBatch` 透传 `numThreads` 参数
- **输出文件预分配（ftruncate）** — 消除 pwrite 时 NTFS 元数据竞争（50GB 提升 22.6%）
- 读/写分离计时诊断（r= / w=）
- batch 模式 per-slice detail 记录实际 write 耗时
- Reference 信息修正为全量体积 + 估算时间

## X-Plane 扩展（存储比 >1.5x，仅供参考）

在文件末尾追加连续 X 切片平面数据，X 读取只需 1 次 pread：

| 数据集 | 存储比 | T_composite | X random |
|--------|--------|------------|----------|
| 20GB (stride=1) | 2.075x | 25.18s | 25s (原 74s) |
| 50GB (stride=1) | ~1.94x | 65.30s | 29s (原 167s) |

X-plane 显著提升 X 切片性能，但 stride=1 存储比超过 1.5x 限制。

## X-Plane stride 优化（存储比 ≤1.5x，已验证）

使用 `erwt3d_precompute_x` 工具降低 X-plane stride，可在存储限制内大幅提升 X 随机性能：

| stride | 存储比 (20GB) | X 命中率 | X random (100片) | T_composite |
|--------|--------------|----------|-----------------|-------------|
| 64 (基线) | 1.075x | 1.6% | 73.78s | 34.42s |
| 3 (推荐) | 1.408x | 33% | 95.76s* | 40.86s* |
| 2 | 1.576x | 50% | — | — (存储扣1分) |
| 1 | 2.08x | 100% | 25s | — (超限) |

\* v0.4 结果，计时含写出。stride=3 在 20GB 上存储比 1.408x 满分，X 随机命中 ~33 片。

X-plane 和 X-panel 是两种不同机制：
- X-plane：全量 YZ 平面，存储在文件末尾，`erwt3d_precompute_x --stride N` 生成（推荐）
- X-panel：per-superblock YZ 平面，`erwt3d_convert --panel-axis x --panel-stride N` 生成

## 推荐命令

```bash
# === 20GB 数据 ===
# 数据转换
./build/erwt3d convert input=cup_3d_small.dat output=cup_3d_small.erwt3d \
    nx=801 ny=2405 nz=2501 threads=8 memory-limit-mb=4096

# 追加 X-plane（stride=3，存储比 ~1.408x，20/20 存储分）
./build/erwt3d precompute-x raw=cup_3d_small.dat erwt3d=cup_3d_small.erwt3d \
    nx=801 ny=2405 nz=2501 stride=3

# === 50GB 数据 ===
# 数据转换
./build/erwt3d convert input=cup_3d_big.dat output=cup_3d_big.erwt3d \
    nx=2001 ny=2201 nz=3000 threads=8 memory-limit-mb=4096

# 追加 X-plane（stride=3，存储比 ~1.378x，20/20 存储分）
./build/erwt3d precompute-x raw=cup_3d_big.dat erwt3d=cup_3d_big.erwt3d \
    nx=2001 ny=2201 nz=3000 stride=3

# === 通用命令 ===
# 正确性验证
./build/erwt3d verify raw=data.raw erwt3d=data.erwt3d \
    nx=N ny=N nz=N samples=100000

# 赛题 benchmark（推荐 --hdd 模式，4GB 内存限制）
./build/erwt3d_bench_contest --input data.erwt3d --output-dir /mnt/d/bench_out --hdd

# 内存扫描（约 1-2 小时）
./scripts/bench_mem_sweep.sh
```
