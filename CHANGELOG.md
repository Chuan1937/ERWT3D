# Changelog

本文件记录 ERWT3D 的重要变更。格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

## [0.6.0] - 2026-07-13

### Added

- **流式 Z-slab 转换**（`tools/erwt3d_convert.cpp`）：
  - 新转换引擎：按 Z 方向 slab（`slab_z=64` 层）分批读取 raw 文件，在内存中完成 Morton 重排和组装，每 slab 转换完即写出 superblock
  - 输入 `raw_read_calls` 精确等于 `ceil(nz / slab_z)`，每 slab 一次大块顺序读，最大化 HDD 顺序带宽
  - 20GB 数据集 `input_reads=40`（=ceil(2501/64)），50GB 数据集 `input_reads=47`（=ceil(3000/64)）
  - 内存占用由 slab 大小决定，20GB 数据集 slab=470MB，50GB 数据集 slab=1075MB，均在管控范围内

- **per-block lz4 压缩决策**：
  - 每个 superblock 独立尝试 lz4 压缩，压缩后体积更小则存压缩数据，否则保留原始数据
  - 20GB 数据集压缩率 0.443x（从 19.3GB → 8.0GB），50GB 数据集接近无压缩 0.996x
  - 压缩索引存于文件末尾，reader 自动适配压缩/非压缩混合布局

### Changed

- **可配置物理超块布局**（`--physical-order`）：
  - `v05-yzx`（默认）：Y-major 优先，Y 切片跨步 32 MB，Y 方向读取更连续
  - `zyx`：旧的 Z-major 布局（Z 优但 Y 切片跨步 ~1120 MB）
  - Smoke test 确认 v05-yzx 全胜：50GB Y_random 253s → 93s（-63%），Z 未退化

- `erwt3d_convert` 新增 `--compress` 选项启用 lz4 压缩
- `erwt3d_convert` 新增 `--scratch-dir` 指定临时目录
- **压缩批量读取优化**（`src/reader.cpp`）：
  - `CompressedReadMode::Windowed`：按 file_offset 排序合并相邻压缩块为窗口读取
  - 默认 windowed 模式，`compressedBuffer_` 复用，减少 pread 次数
  - 新增 `--compressed-read-mode v051|windowed` 开关

### Changed

- **修复 verify 工具**（`tools/erwt3d_verify.cpp`）：
  - 采样模式下 raw 文件读取从逐点 `pread`（1M 样本 = 1M 次随机 HDD seek，~数小时）改为 mmap 零拷贝访问
  - 按 linIdx 排序后顺序访问 mmap 区域，消除 HDD 随机寻道瓶颈
  - 添加进度输出

### Reader A/B 验证

| 模式 | 20GB Y_random | 说明 |
|------|:---:|------|
| V051（逐块 pread） | 58.24s | 19760 次独立 pread |
| **Windowed（窗口合并）** | **45.53s** | ~68 次窗口 pread，4x 减少 |

### Performance

D 盘 HDD，`--hdd` 模式，4GB 内存限制，**输入输出均在 HDD**：

| 数据集 | 配置 | v0.5.1 best | v0.6.0 (cold) | **v0.6.0 (warm)** |
|--------|------|:---:|:---:|:---:|
| 20GB | lz4 + ZYX + sidecar | **17.49s** | 30.84s | 18.78s |
| 50GB | lz4 + ZYX | **104.74s** | 122.71s | 106.08s |

注：v0.6.0 读取更快（20GB X 读 6.2s vs v0.5.1 ~15s），HDD 读写竞争抵消部分收益。50GB 接近追平（+1.3%）。接入 LeafOp + 写优化可望超越。

### Conversion stats

| 数据集 | 配置 | slab_z | input_reads | raw_bytes_read | 转换时间 | 峰值内存 | 物理布局 |
|--------|------|--------|-------------|---------------|----------|----------|----------|
| 20GB | lz4 | 64 | 40 | 19,271,755,620 | 364s | ~1097 MB | v05-yzx |
| 50GB | lz4 | 64 | 47 | 52,850,412,000 | 2015s | ~1099 MB | v05-yzx |

### ZYX vs v05-yzx 对照 (smoke, 10r/3c, cold cache)

| 轴 | 20GB ZYX | 20GB yzx | 50GB ZYX | 50GB yzx |
|----|:---:|:---:|:---:|:---:|
| X random | 3.1s | **1.0s** | 253.6s | **93.0s** |
| Y random | 11.6s | 11.2s | 115.6s | **62.9s** |
| Z random | 12.0s | 12.2s | 80.3s | **49.0s** |
| T_composite | 5.0s | **4.5s** | 82.1s | **37.1s** |

**结论**：v05-yzx 在三轴均等价或优于 ZYX，50GB 全轴 -39%~-63%。v0.6.0 默认使用 v05-yzx。

### Notes

- 测试方法论：smoke test（10r/3c）快速筛选举局，确认优势后才跑完整 100/10
- ZYX 旧文件的 sidecar flag 未写入 header（v0.5.1 兼容问题），修复后才有公平对比
- 新 Z-slab 路径仅用于无 panel 转换；panel 模式自动回退到旧的兼容路径

## [0.5.1] - 2026-07-07

### Changed

- **流式 sidecar writer**（`tools/erwt3d_precompute_x.cpp`）：
  - 不再把所有 X plane 全放内存，改为按 z-chunk 分批处理
  - 每个 chunk 覆盖 `chunk_z_rows` 个 z 层，扫描后立即压缩写出
  - 内存从 `planeCount × ny × nz × 4` 降至 `planeCount × ny × chunkZRows × 4`（20G stride=1: 18GB → 1.9GB）
  - legacy 模式也改为逐 plane 流式提取

- **sidecar batch reader**（`src/reader.cpp`）：
  - `tryReadBatchXPSidecar_`：把所有命中 sidecar 的 X 切片展开为 chunk task，按 `chunk_offset` 全局排序，4KB gap 容忍合并连续 chunk 为单次 pread
  - 减少 HDD 寻道：100 个 X random 命中时从 100×10=1000 次 pread 降至按物理顺序合并的少量窗口读

- **复用读写缓冲**（`src/reader.cpp`）：
  - `xpCompBuf_`/`xpRawBuf_` 挂在 reader 上长期复用，不再每片/每 chunk 反复分配

- **stride 自动决策改为预算搜索**（`tools/erwt3d_precompute_x.cpp`）：
  - 不再硬编码 `stride < 8` 上限，从 stride=1 递增直到满足 budget 或超过 nx
  - 新增 `--storage-budget` 参数（默认 1.45）

### Performance

D 盘 HDD，`--hdd` 模式，4GB 内存限制，best run：

| 数据集 | 配置 | T_composite | 存储比 | vs 旧基线 |
|--------|------|-------------|--------|-----------|
| 20GB | lz4 + sidecar stride=1 | **17.49s** | 0.932x | **-25.8%** (旧 23.56s) |
| 50GB | lz4 (无 sidecar) | **104.74s** | 0.996x | **-6.0%** (旧 111.39s) |

50G sidecar stride=3 测试结果：T_composite 154s（恶化），原因是 sidecar 文件 16GB 占用 page cache 干扰 Y/Z random。50G 数据集压缩率太差（0.979x），不适合 sidecar。

### Notes

- 参数扫描：chunk_z_rows 64/128/256/512/1024 压缩率差异 <0.2%，256 是合理默认值
- 50G stride 扫描：stride=3 可满足 1.45x（1.31x），但 sidecar 16GB 导致 page cache 干扰，净效果为负

## [0.5.0] - 2026-07-07

### Added

- **X-plane 压缩 sidecar**（`data.erwt3d.xp`）：
  - 独立 sidecar 文件，不污染主文件数据布局
  - 每个 X-plane 按 Z 方向分段（默认 256 行/chunk），lz4 独立压缩
  - 自动 stride 决策：从 stride=1 开始递增（100% 命中），存储超 budget 则增大 stride，直到满足或超过 nx 则跳过（v0.5.1 修正：原 v0.5.0 逻辑反转，已修复）
  - 文件尾部带 chunk 索引（`plane_id → [chunk_offset, chunk_size, raw_size]`），单次 pread 加载
  - 主文件头仅新增 `FLAG_HAS_XP_SIDECAR`（bit 5）+ `reserved[21]` 哨兵
  - Reader 构造时懒加载 sidecar，命中走快路径（pread + LZ4 解压 → memcpy 到输出），miss 降级到 SB 路径
  - `erwt3d_precompute_x` 新增 `--mode sidecar`（默认）/ `--mode legacy`、`--stride`、`--chunk-z-rows` 参数

### Changed

- **LeafOp 紧凑化**（`include/erwt3d/sb_plan.hpp`）：
  - per-leaf 表示从 48 字节（`leaf_data` 4×u64 + `leaf_out` 4×u32）压缩为 16 字节的 `LeafOp` 结构
  - `LeafOp { out_base:u32, out_stride:u32, morton:u16, param:u8, v_inner:u8, v_outer:u8, pad[3] }`
  - `SBTaskPlan.leaf_data` + `leaf_out` 合并为 `leaf_ops`，`first_leaf` 直接索引（无需 ×4）
  - `sortTasksByFileOffset` 从双向量搬运简化为单向量 `insert(range)`
  - 影响文件：`sb_plan.hpp`、`sb_common.cpp`（plan builders + unpackLeaves + sort）、`sb_hdd.cpp`（LeafIndex 路径）

- **存储比计入 sidecar**（`erwt3d_bench_contest.cpp`、`erwt3d_info.cpp`、`api.cpp`）：
  - bench-contest 和 info 的存储比计算现在累加 `.xp` sidecar 文件大小
  - 修复了 sidecar 存在时存储比被低计的评分 bug

### Performance

D 盘 HDD，`--hdd` 模式，4GB 内存限制，best run：

| 数据集 | 配置 | T_composite | 存储比 | vs 旧基线 |
|--------|------|-------------|--------|-----------|
| 20GB | lz4 + sidecar stride=1 | **17.49s** | 0.932x | **-25.8%** (旧 23.56s, lz4 无 sidecar) |
| 50GB | lz4 (LeafOp 重构) | **104.74s** | 0.996x | **-6.0%** (旧 111.39s, lz4 无 LeafOp) |

注：旧基线为 v0.4 的 lz4 压缩版本（无 sidecar、无 LeafOp 紧凑化）。v0.4 的无压缩 + X-plane stride=3 版本为 37.99s（20G）/ 105.91s（50G），不可直接对比。

| 测试组 | 旧基线 (lz4) | sidecar stride=1 | 提升 |
|--------|-------------|------------------|------|
| X random (100片) | 62.99s | 15.05s | **-76.1%** |
| X continuous (10片) | 3.68s | 1.59s | **-56.8%** |
| Y random (100片) | 43.48s | 47.00s | +8.1% |
| Z random (100片) | 35.23s | 41.42s | +17.6% |

50GB sidecar 因数据压缩率差（0.94x）自动跳过，仅 LeafOp 重构收益。

### Notes

- 20GB 数据集 sidecar 压缩率 0.489x（YZ 平面空间相关性），stride=1 总存储比 0.932x
- 50GB 数据集 sidecar 压缩率 0.942x，stride=2 总存储比 1.467x 超 1.45x 安全阈值，工具正确跳过
- X random 的改善主要来自 sidecar hit 时只需读取压缩 chunk（~11MB/plane）而非扫描整个文件（~8.5GB）
- sidecar 生成采用顺序扫描 raw 文件（按 z 层读取 nx*ny 个 float，抽取 x 列），生成阶段不计时

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
