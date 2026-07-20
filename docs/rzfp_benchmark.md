# RZFP 基准测试结果

## v0.7.0（P5 Round-Level Joint Planning）

`erwt3d_contest` 入口，stable-auto 协议。

测试环境：i9-10850K（8C/16T）、62 GiB RAM、G 盘 HDD via WSL2 9p、HEAD `edd6f2a`、`--threads 8`、`--seed 20260511`。

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

稳定性（stable-auto x5）：AUTO mean 40.01s / median 39.98s / min 39.82s / max 40.32s / CV 0.5%；M8 mean 41.34s / median 41.33s / min 41.21s / max 41.47s / CV 0.3%。cold-round x3：AUTO 40.27s、M8 41.68s。

### 20GB RZFP + X-plane sidecar（1.036x）

| 配置 | 内存 | T_composite | RSS |
|------|------|-------------|-----|
| AUTO | 17 GiB | **16.79s** | 17.8 GiB |
| **M4** ★ | **4 GiB** | **17.25s** | **8.7 GiB** |
| M2 | 2 GiB | 50.44s | 4.7 GiB |

### P4 vs P5（50GB M8）

| 模式 | mean |
|------|------|
| P4 (p4-groups) | 42.36s |
| P5 (p5-round) | 41.83s ¹ |

¹ P5 round 2 outlier 45.4s excluded (disk activity)。

### 同盘/异盘（50GB M8）

| 模式 | mean |
|------|------|
| 同盘 (G→G) | 41.60s |
| 异盘 (G→WSL) | **37.69s** |

### 关键验证

- 21/21 CTest 通过
- 2GB 不崩溃，decode errors = 0
- AUTO vs M8 hash 一致
- 输出文件 330/330
- 存储比 ≤ 1.50（50GB 1.421x，20GB 1.036x）
- 5 轮 CV ≤ 0.5%

### 推荐参赛配置

|  | 50GB | 20GB |
|---|------|------|
| 推荐 | M8 (8 GiB, 128 MB) — 41.37s | M4 (4 GiB) — 17.25s |
| 绝对最快 | AUTO (27 GiB) — 39.95s | AUTO (17 GiB) — 16.79s |
| 低内存 | M2 (2 GiB) — 88.30s | M2 (2 GiB) — 50.44s |

## 历史结果（v0.6.0，Z-fastest 布局修正后）

所有时间均为 G 盘 HDD 上的 wall-clock 读取+写入时间，Z-fastest 布局修正后重新生成并验证。

## 20GB 真实数据（801×2405×2501）

### RZFP 主文件

```
T_x_random:      177.1s
T_y_random:       63.6s
T_z_random:       56.8s
T_x_continuous:    6.06s
T_y_continuous:    2.03s
T_z_continuous:    1.85s
T_composite:      51.26s
storage_ratio:     0.607x (10.9 GB)
verification:      fast-full 4.8B points, 0 failures, max_rel_error=0.001
```

### LZ4 主文件

```
T_x_random:      129.3s
T_y_random:       39.5s
T_z_random:       39.9s
T_x_continuous:    3.96s
T_y_continuous:    1.40s
T_z_continuous:    1.32s
T_composite:      35.89s
storage_ratio:     0.432x (7.8 GB)
verification:      100K random points, 0 failures
```

### LZ4 + X-plane sidecar stride=1

```
T_x_random:       12.7s
T_y_random:      102.1s
T_z_random:       44.5s
T_x_continuous:    1.25s
T_y_continuous:    1.25s
T_z_continuous:    1.37s
T_composite:      27.19s
storage_ratio:     0.526x (7.8 GB + 1.7 GB)
```

### LZ4 + X-plane sidecar stride=2 （推荐）

```
T_x_random:       45.4s
T_y_random:       40.9s
T_z_random:       39.3s
T_x_continuous:    4.53s
T_y_continuous:    1.43s
T_z_continuous:    1.39s
T_composite:      22.09s
storage_ratio:     0.479x (7.8 GB + 868 MB)
```

## 50GB 真实数据（2001×2201×3000）

### RZFP 主文件（推荐）

```
T_x_random:      317.6s
T_y_random:      139.3s
T_z_random:      118.8s
T_x_continuous:    5.63s
T_y_continuous:    9.29s
T_z_continuous:    3.72s
T_composite:      99.06s
storage_ratio:     0.421x (21 GB)
verification:      fast-full 13.2B points, 0 failures, max_rel_error=0.001
```

### LZ4 主文件

```
T_x_random:      453.7s
T_y_random:      184.3s
T_z_random:      174.0s
T_x_continuous:    7.75s
T_y_continuous:    6.15s
T_z_continuous:    4.69s
T_composite:     138.41s
storage_ratio:     1.044x (52 GB, 未压缩)
```

## 推荐方案

| 数据集 | 推荐格式 | T_composite | 存储比 | 说明 |
|--------|----------|------------|--------|------|
| 20GB | LZ4 + sidecar s2 | 25.37s (均值) / 22.09s (最佳) | 0.479x | 3轮重复 CV≈2%，22.09s 为热缓存 |
| 50GB | RZFP | 99.06s (单次代表性) | 0.421x | 多轮冷/热缓存重复待补充 |

### Stride 对比（20GB LZ4）

| Stride | T_composite | 存储比 | Sidecar | X_rand | Y_rand | Z_rand |
|--------|------------|---------|---------|--------|--------|--------|
| s1 | 27.19s | 0.526x | 1.7 GB | 12.7s | 102.1s | 44.5s |
| s2 | 25.37s | 0.479x | 868 MB | ~55s | ~47s | ~42s |
| s3 | 24.60s | 0.464x | 578 MB | 54.2s | 48.6s | 38.0s |

Stride=2 最优，平衡命中率（50%）、sidecar 大小与 page cache 压力。

## 自动规划器验证

使用 `erwt3d_auto_plan`，基于 LZ4 Probe + RZFP AutoPlan + HDD 校准：

| 数据集 | Planner 推荐 | 实际最优 | 一致？ |
|--------|-------------|---------|-------|
| 20GB | LZ4 + sidecar s2 (ratio=0.419) | LZ4 + sidecar s2 | ✓ |
| 50GB | RZFP main (ratio=0.420) | RZFP main | ✓ |

## 测试环境

- 磁盘：G 盘 HDD，顺序读约 200-220 MB/s
- CPU：16 线程
- 内存限制：4 GB（正式 benchmark）
- 编译器：GCC 15，-O3 -march=native
- 布局修正：Z-fastest raw → internal X-fastest Leaf

## Planner 限制说明

Planner 使用实测压缩探针和校准后的 HDD 特征对候选格式进行排序。
预测时间为启发式估计，用于格式选择，非精确 benchmark 预报。
sidecar 压缩比在当前实现中由主文件 LZ4 比率近似推导，
对新数据集可作为筛选参考，最终方案仍建议实测验证。
对当前两个比赛数据集，Planner 结果已验证与实际一致。

## 可选后续研究

以下项目不影响当前格式选择和比赛方案，属于可选的后续优化：

- 分离冷缓存与热缓存，进行更多轮重复实验；
- 对 50GB 数据集执行多轮稳定性测试；
- 测试 stride=4（stride=1/2/3 已比较，s2 最优）；
- 对 window/gap/threads 参数做单因素邻域调优；
- 实现 sidecar 独立压缩探针替代当前启发式估算；
- blocked transpose 块大小微调（16×64 vs 32×32 vs 64×16）。
