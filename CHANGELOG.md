# Changelog

本文件记录 ERWT3D 的重要变更。格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

## [0.4.0] - 2026-07-06

### Added

- sw4 风格统一入口 `build/erwt3d`：读取配置文件或直接命令行，替代原有的多个独立二进制
- 配置文件模式：支持 sw4 风格分段配置（`command key=value`），空行分隔任务
- 直接命令模式：`build/erwt3d convert input=... output=...`，不再需要 `--key value` 长参数
- `tools/erwt3d_main.cpp` 作为 C++ 单一入口，自动分派到各子工具

### Changed

- 废弃根目录 `erwt3d` shell 脚本，改为 `build/erwt3d` C++ 二进制
- 全部 `scripts/` 脚本改用 `build/erwt3d <command> key=value` 格式
- 压缩文件 `readSliceSB` 路径按物理偏移排序（`compIndex[].file_offset`），避免 HDD 随机寻道
- `readSlicesBatch` 不再硬编码单线程，响应传入的 `numThreads` 参数

### Fixed

- 压缩文件 HDD 读路径未按物理偏移排序导致大量随机寻道
- `readSlicesBatch` 忽略 `numThreads` 参数，一直单线程
- batch 模式 `contest_detail.csv` per-slice 时间全为 0
- Reference 消息比较字节数而非时间，完全无参考意义

### Removed

- 根目录 `erwt3d` shell 脚本（由 `build/erwt3d` 替代）

### Performance

D 盘 HDD 实测（存储比 ≤1.5x，`--hdd` 模式，计时含文件写出）：

| 数据集 | T_composite | 存储比 | 存储分 |
|--------|------------|--------|--------|
| 20GB (801×2405×2501) | 40.86s | 1.408x | 20/20 |
| 50GB (2001×2201×3000) | 136.81s | 1.378x | 20/20 |

**注意**：v0.4 计时包含文件写出（符合比赛评分口径）。此前版本仅计读取时间，不可直接对比。

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
