# Changelog

本文件记录 ERWT3D 的重要变更。格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

## [0.3.0] - 2026-06-25

### Added

- `erwt3d_precompute_x` 工具：从 RAW 文件生成连续 X-plane 数据追加到 ERWT3D 文件末尾，支持 `--stride N` 参数控制存储密度
- `readSlicesBatch` 中 X-plane 切片走单次 pread 快速路径，Y/Z 切片走正常 batch 路径
- `format.hpp` 新增 `FLAG_HAS_X_PLANES`、`getXPlaneOffset`、`getXPlaneCount`、`getXPlaneStride`

### Changed

- readahead 前瞻窗口从 3 增大到 10，I/O 流水线更深（5-7% 提升）

### Performance

D 盘 HDD 实测（存储比 ≤1.5x，`--hdd` 模式）：

| 数据集 | T_composite | 存储比 |
|--------|------------|--------|
| 20GB | 34.42s | 1.075x |
| 50GB | 87.67s | 1.044x |

带宽利用率 93.6%，Z 轴 99%、Y 轴 91%、X 轴 77%（受限于 9p pread 开销）。

## [0.2.0] - 2026-06-24

### Changed

- **batch size 动态计算**：`erwt3d_bench_contest` 的 batch size 从固定 20 改为根据 `--memory-limit-mb` 动态计算，优先将同组所有切片放入单批次。扣除了读缓冲区开销，确保不超出用户指定的内存限制。
- 跨切片全局排序后合并读窗口，消除不同批次间对同一 superblock 的重复读取。

### Performance

| 数据集 | 旧 T_composite | 新 T_composite | 提速 |
|--------|---------------|---------------|------|
| 20GB | 94.11s | 34.42s | 2.73x |
| 50GB | 223.43s | 87.67s | 2.55x |

### Memory Limit Sweep

| MemLimit | 20GB T_composite | 50GB T_composite |
|----------|-----------------|-----------------|
| 2 GB | 45.73s | 121.74s |
| 4 GB | 34.42s | 87.67s |
| 8 GB | 34.41s | 89.00s |
| 16 GB | 34.66s | 88.24s |
| 32 GB | 34.50s | 87.59s |
| 64 GB | 34.49s | 89.21s |

4GB 是拐点，≥4GB 后 all-in-one batch，性能稳定。

## [0.1.0] - 初始版本

### Features

- Superblock (64³) → Leaf (4³) 两级层次结构
- Morton Z-order 物理布局，三轴访问均衡
- HDD 优化读取：128MB 读窗口 + 1MB gap 容忍 + 文件偏移排序
- X-Panel 预存面板加速 X 切片访问
- 赛题2 标准评分工具 `erwt3d_bench_contest`
