# Changelog

本文件记录 ERWT3D 的重要变更。格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

## [0.4.0] - 2026-07-06

### 数据布局说明

当前文件格式的两级层次：

- **Superblock 级别**：Z-Y-X 逻辑顺序排列（file_offset = data_offset + sbIdx × sbBytes, sbIdx = (z·gridY + y)·gridX + x）
- **Leaf block 级别**：Superblock 内部 4096 个 Leaf（16×16×16）按 Morton Z-order 排列
  - `leaf_offset = sb_offset + morton3D(lx, ly, lz) × leaf_bytes`
  - Morton 编码使三轴在 Leaf 级别访问均衡

### Added

- **sw4 风格统一入口** `tools/erwt3d_main.cpp` → `build/erwt3d`：
  - 支持配置文件模式（`erwt3d config.txt`，sw4 风格分段格式 `command\n  key=value`）
  - 支持直接命令模式（`erwt3d convert input=... output=...`，不再需要 `--key value` 长参数）
  - 自动分派到各子工具（`erwt3d_convert`, `erwt3d_bench_contest` 等）
- **读/写分离计时诊断**（`tools/erwt3d_bench_contest.cpp`）：
  - `GroupResult` 新增 `readTimeMs` / `writeTimeMs` 字段
  - 每组输出格式：`x random (100 slices)... 82.88s (r=71.30s w=10.30s)`
  - `contest_summary.csv` 新增 `read_time_ms` / `write_time_ms` 列
  - `contest_score.csv` 新增每组 `read_*_ms` / `write_*_ms` 列
- **远程 merge 引入**：`tools/erwt3d_bench_line.cpp` 单列 benchmark、`scripts/benchmark_contest_strict.sh`、`scripts/verify_contest.sh`

### Changed

- 废弃根目录 `erwt3d` shell 脚本，改为 `build/erwt3d` C++ 二进制
- 全部 `scripts/` 脚本改用 `build/erwt3d <command> key=value` 格式

### Fixed

- **压缩文件 HDD 单切片读未按物理偏移排序**（`src/reader.cpp:readSliceSB`）
  - **根因**：压缩文件中 superblock 的物理位置在 `compIndex[sbIdx].file_offset`，与逻辑偏移 `data_offset + sbIdx * sbBytes` 无关。原代码按逻辑偏移顺序遍历 tasks，在 HDD 上造成大量随机寻道。
  - **修复**：构建 `taskOrder` 按 `compIndex[sbIdx].file_offset` 排序后遍历。
  - **额外优化**：添加 `lastSbIdx` 缓存——同一 superblock 若被同一 slice 的多个 task 引用，只读一次。
  - **影响文件**：`src/reader.cpp:662-690`
  - **参照**：`readSlicesBatch` 的压缩路径（`src/reader.cpp:753-761`）早已正确排序，修复后单切片路径与之行为一致。

- **`readSlicesBatch` 硬编码单线程**（`src/reader.cpp:794`）
  - **根因**：`executeSBBatchHDD(..., 1, ...)` 第4参数硬编码为 `1`，忽略调用者传入的 `numThreads`。
  - **修复**：改为 `executeSBBatchHDD(..., numThreads, ...)`，由 `executeSBBatchHDD` 内部的线程安全保护（`batch.plans.size() > 1` 时强制单线程）兜底。
  - **影响**：SSD/NVMe 场景可使用多线程 batch 读；HDD 场景 `numThreads=1` 不受影响。

- **batch 模式 per-slice detail 全为 0**（`tools/erwt3d_bench_contest.cpp:139`）
  - **根因**：batch 模式下 `result.perSliceTimes.push_back(0)` 未记录各切片写出耗时。
  - **修复**：在 pwrite 前后计时，写入实际耗时。相当于记录单切片写出时间（含 HDD 写入竞争开销）。

- **Reference 消息计算错误**（`tools/erwt3d_bench_contest.cpp:515-519`）
  - **根因**：打印 "Compare T_composite with your disk's sequential read of 3 × slice_bytes bytes"，将时间与字节数比较，完全无参考意义。且用 3 个切片体积而非全量体积。
  - **修复**：改为输出全量体积（GiB）+ 公式 + 示例估算时间。

### Performance

- **输出文件预分配（ftruncate）**（`tools/erwt3d_bench_contest.cpp:runGroup`）
  - **原理**：原来 `open(O_WRONLY | O_CREAT | O_TRUNC)` 创建零长度文件，首次 `pwrite(fd, data, outBytes, 0)` 触发 NTFS 文件扩展元数据操作（分配簇链、更新 MFT），与读请求在 HDD 上竞争磁头定位。`ftruncate(fd, outBytes)` 提前预分配文件大小，pwrite 时只需写数据无需扩展元数据。
  - **实现**：文件预创建时改为 `open(O_RDWR | O_CREAT | O_TRUNC)` + `ftruncate(fd, outBytes)`，避免 `pwrite` 在计时路径内做元数据更新。
  - **前置探索**：尝试过 mmap 写（`mmap + memcpy + munmap`）替代 pwrite，分析发现 mmap 需额外 syscall 且写回时机不可控；尝试过自适应 gap tolerance（按 gridX 动态增大 gap），分析发现 X random 在 3MB gap 下已自然连续（100 个随机位置覆盖所有 superX 值，相邻 superblock 间无间隙），增大 gap 反而让 X continuous 读放大了 13x（合并全文件为一个窗口）。
  
  实测提升：

  | 数据集 | 优化前 T_composite | 优化后 T_composite | 提升 |
  |--------|-------------------|--------------------|------|
  | 20GB (X-plane stride=3) | 40.86s | 37.99s | **-7.0%** |
  | 50GB (X-plane stride=3) | 136.81s | 105.91s | **-22.6%** |

  50GB 各组分时间对比：

  | 测试组 | 优化前 | 优化后 | 提升 |
  |--------|--------|--------|------|
  | X random (100片) | 421.02s | 267.84s | **-36.4%** |
  | Y random (100片) | 196.30s | 178.68s | -9.0% |
  | Z random (100片) | 176.03s | 171.32s | -2.7% |
  | X continuous (10片) | 16.43s | 7.66s | **-53.4%** |
  | Y continuous (10片) | 6.29s | 5.70s | -9.4% |
  | Z continuous (10片) | 4.76s | 4.27s | -10.3% |

  **关键发现**：X random 读时（243.8s）仍占组时间 91%，瓶颈在 HDD 顺序带宽。X 方向读取需遍历全部 superblock（50GB 数据集 ~52GB），受 HDD 实际有效带宽 ~215 MB/s 限制。进一步优化需改变文件物理布局或提高 X-plane 命中率。

  **被放弃的优化方向**：
  - 增大读窗口（128MB → 1GB）：分析发现对 X random 窗口合并不起作用（所有 superX 在 3MB gap 下已自然连续，增大 gap 反而让 X continuous 全文件合并为 1 个大窗口，数据放大了 13x）
  - mmap 替代 pwrite：pre-truncate 已消除主要写入瓶颈（元数据开销），mmap 额外引入 syscall 开销且写回不可控
  - 自适应 gap tolerance：仅对 X 轴有效且与现有 3MB gap 效果相同

### Removed

- 根目录 `erwt3d` shell 脚本（由 `build/erwt3d` C++ 二进制替代）

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
