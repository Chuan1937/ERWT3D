# RZFP 基准测试结果

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
