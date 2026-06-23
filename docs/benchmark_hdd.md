# HDD 性能测试报告

## 测试环境

| 项目 | 配置 |
|------|------|
| CPU | Intel i7-13700F, 24 线程 |
| RAM | 64 GB |
| 存储 | WSL 9p 挂载（模拟 HDD 随机读特性） |
| 数据集 | small (801×2405×2501, 18GB raw) |

## 评分公式

```
T_composite = (T_xr + T_yr + T_zr + T_xc + T_yc + T_zc) / 6
性能得分 = (基准时间 / T_composite) × 60
存储得分：≤1.5x → 20分，每超10% → 扣1分
```

## 正确性验证

```
samples:      100000
max_abs_err:  0
max_rel_err:  0
failed:       0
结果:         PASSED
```

## 存储指标

| 格式 | 大小 | 比率 | 得分 |
|------|------|------|------|
| 原始 raw | 18.0 GB | — | — |
| erwt3d (无panel) | 19.3 GB | 1.075x | 20/20 |
| erwt3d (panel stride=4) | 24.2 GB | 1.344x | 20/20 |

## 读取模式对比

单线程，10 随机 + 5 连续，batch planner 开启。

| 模式 | X随机 | Y随机 | Z随机 | X连续 | Y连续 | Z连续 | T_total |
|------|-------|-------|-------|-------|-------|-------|---------|
| pread | 6979ms | 1967ms | 1869ms | 1723ms | 426ms | — | — |
| run-batch | 5012ms | 1592ms | 1756ms | 1119ms | 345ms | 333ms | 1693ms |
| **hdd-read-window** | **4699ms** | **1541ms** | **1592ms** | **1086ms** | **362ms** | **335ms** | **1602ms** |

**结论：hdd-read-window 最优，比 pread 快 ~30%。**

## 线程数对比

HDDReadWindow 模式，10 随机 + 5 连续。

| 线程数 | T_total |
|--------|---------|
| 1 | 2174ms |
| 2 | 1678ms |
| 4 | 1592ms |
| 8 | 1527ms |

**结论：8 线程比单线程快 ~30%。9p 环境下多线程仍有收益（与原生 HDD 不同）。**

## 缓存对比

8 线程，HDDReadWindow，10 随机 + 5 连续。

| 缓存 | T_total |
|------|---------|
| 0 MB | 1702ms |
| 256 MB | 1629ms |
| 512 MB | 1733ms |
| 1024 MB | 1574ms |

**结论：缓存效果有限，1024MB 最优但仅提升 ~7%。**

## 完整 100+10 测试结果

### 无 Panel（存储比 1.075x）

| 配置 | X随机 | Y随机 | Z随机 | X连续 | Y连续 | Z连续 |
|------|-------|-------|-------|-------|-------|-------|
| t=1, batch | 750ms | 669ms | 615ms | 629ms | 186ms | 195ms |
| **t=8, batch** | **721ms** | **639ms** | **664ms** | **620ms** | **202ms** | **196ms** |

### 有 Panel stride=4（存储比 1.344x）

| 配置 | X随机 | Y随机 | Z随机 | X连续 | Y连续 | Z连续 |
|------|-------|-------|-------|-------|-------|-------|
| t=1, batch | 762ms | 699ms | 640ms | 629ms | 194ms | 170ms |

**结论：Panel 在 9p 环境下无明显收益（瓶颈是协议开销，非数据量）。原生 HDD 上预期有显著改善。**

## 推荐配置

| 环境 | 配置 |
|------|------|
| HDD 竞赛 | `--threads 1 --sb-read-mode hdd-read-window --sb-task-order file-offset --hdd-read-window-bytes 33554432 --hdd-max-gap-bytes 1048576 --hdd-batch-planner on` |
| SSD 竞赛 | `--threads 8 --sb-parallel-mode parallel-read --sb-task-order file-offset` |

## 竞赛基准时间参考

根据官方说明，基准时间 ≈ 主维顺序读时间 × 3。

small 数据集主维（X）顺序读：
- 每片 2405×2501×4 = 22.9 MB
- 3 片 = 68.7 MB
- HDD 顺序读 ~150 MB/s → 约 0.46s

预期基准时间 ≈ 0.5-1.0s。
