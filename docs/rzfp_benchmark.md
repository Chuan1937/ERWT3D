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

| 数据集 | 推荐格式 | T_composite | 存储比 |
|--------|----------|------------|--------|
| 20GB | LZ4 + sidecar s2 | 22.09s | 0.479x |
| 50GB | RZFP | 99.06s | 0.421x |

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

## 尚未完成的基准工作

- 3 冷 + 3 热缓存重复性测试
- stride=3 和 stride=4 的快速比较
- 不同 HDD 窗口参数（window/gap/threads）的单因素邻域优化
