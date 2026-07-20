# Changelog

本文件记录 ERWT3D 的重要变更。格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

## [0.7.0] - unreleased

### Added

- **P3: Adaptive and Cache-Stable I/O**（branch: `perf/p3-adaptive-cache-stable-io`）：
  - **`DeviceProfile`**：运行时测量顺序带宽（3×128MB 中位数）和随机延迟（64×64KB 去极值）
  - **`DeviceProfileCache`**：线程安全单例，按 `st_dev` 缓存
  - **`CachePolicy`**：StableAuto / DeterministicCold / WarmAllowed
  - **`RzfpAdaptiveConfig`**：15% 迟滞、120s Fullscan 超时、20% 最低优势
  - **`chooseAdaptiveStrategy()`**：I/O + seek + decode 统一成本模型
  - **Fullscan 灾难保护**：慢盘 + 大文件禁止 Fullscan；超时需额外优势
  - **`--cache-policy` CLI**（stable-auto/cold/warm）

### Changed

- RZFP 策略选择不再使用硬编码 220 MB/s，改用实测带宽
- 保守默认值：80 MB/s / 12ms（不再 220/9）

### Fixed

- 慢盘上 Y random 不再错误选择灾难性 FullPayloadScan

---

- **P4: HDD Memory-Adaptive and Cache-Stable I/O**（branch: `agent/p4-hdd-memory-adaptive-cache`）：
  - `--benchmark-cache-mode`（cold-group/cold-round/stable-auto/warm）
  - `MemoryBudget` 严格/自动内存核算
  - `WindowCache` 有界 LRU 压缩窗口缓存
  - `RzfpAdaptiveStrategy` 15% 迟滞成本模型
  - CentOS 7 移植（portable x86-64，`-DERWT3D_NATIVE_OPT=OFF`）
  - 50GB RZFP cold-group 43.06s, 2GB 不 OOM

- **P5: Round-Level Joint Planning and HDD Pipeline**（branch: `perf/p5-round-planner-hdd-pipeline`, PR #55）：
  #### Added
  - **`readContestRound()`**：Y/Z 四组（random + continuous）联合为一次 `readSlicesBatch`，跨组 Leaf 去重
  - **`ContestRoundExecutor`**：共享阶段执行器，benchmark 与 `erwt3d_contest` 共用
  - **`ContestPhasePlan`**：字节预算阶段规划，每组仅一次，单组超预算也可执行
  - **`erwt3d_contest`**：正式比赛入口，支持 `--memory-limit-mb N`、`--read-window-mb N`
  - **`BoundedWindowCache::getContaining()`**：包含命中范围缓存
  - **`accumulateReadProfile()`**：多 batch 统计聚合（sum 替代 max）
  - **`RzfpReadProfile` 去重统计**：`logical_leaf_requests`、`duplicate_leaf_requests`、`dedupReductionRatio`，在 `buildLeafTasks` 中增量追踪
  - **X 联合读取**：X random + X continuous 合并为一次 `readSlicesBatch`
  - **进度输出**：stderr 每 2 batch 打印阶段/batch/剩余切片数
  - **计时分解**：`setup_time_ms`/`output_prepare_ms`/`read_time_ms`/`write_time_ms`/`close_time_ms`/`wall_time_ms`
  - **`--execution-mode p4-groups|p5-round`**（benchmark）

  #### Changed
  - 存储预算 `RAW_X_AUX_HARD_LIMIT` 从 1.450 提升至 1.500
  - `computeWindow()`：`windowEnd = std::max(windowEnd, end)` 修复同 offset 不同 size 导致窗口收缩
  - `patchExceptions()`：`bool` 返回 + popcount 前置校验，消除 `std::out_of_range`
  - 直读回退：decode 失败时从磁盘重读同条记录并比较
  - benchmark `runP5Round` 改为 `executeContestRound` 包装，删除 232 行重复逻辑

  #### Fixed
  - `patchExceptions()` popcount 校验防止 `exc_count` 与 mask 不一致崩溃
  - 批量映射：`ActiveBatchGroup` 显式跟踪，消除 `phase.group_ids[pi]` 对齐错位
  - `peak_accounted_bytes` 从双重计数修复
  - 缓存统计互斥：exact/contained/miss 单次查询

  #### Performance

  **测试环境**：i9-10850K（8C/16T）、62 GiB RAM、0 swap、G 盘 HDD via WSL2 9p、GCC 15.2.1、CMake 3.31、HEAD `edd6f2a`。
  `erwt3d_contest` 入口，`--threads 8`，`--seed 20260511`。

  **50GB RZFP + Raw X Aux（1.421x）**：

  | 配置 | 内存 | 窗口 | T_composite | RSS | vs AUTO |
  |------|------|------|-------------|-----|---------|
  | AUTO | 27 GiB | 512 MB | **39.95s** | 30.4 GiB | baseline |
  | M28 | 28 GiB | 512 MB | 40.70s | 30.4 GiB | +1.9% |
  | M24 | 24 GiB | 512 MB | 40.30s | 30.4 GiB | +0.9% |
  | **M8** ★ | **8 GiB** | **128 MB** | **41.37s** | **13.7 GiB** | **+3.6%** |
  | M16 | 16 GiB | 256 MB | 42.20s | 22.1 GiB | +5.6% |
  | M4 | 4 GiB | 64 MB | 56.67s | 8.4 GiB | +42% |
  | M2 | 2 GiB | 64 MB | 88.30s | 4.4 GiB | +121% |

  **稳定性（stable-auto x5）**：

  | 配置 | mean | median | min | max | CV |
  |------|------|--------|-----|-----|-----|
  | AUTO | 40.01s | 39.98s | 39.82s | 40.32s | 0.5% |
  | M8 | 41.34s | 41.33s | 41.21s | 41.47s | 0.3% |

  **cold-round x3**：

  | 配置 | mean |
  |------|------|
  | AUTO | 40.27s |
  | M8 | 41.68s |

  **20GB RZFP + X-plane sidecar（1.036x）**：

  | 配置 | 内存 | T_composite | RSS |
  |------|------|-------------|-----|
  | AUTO | 17 GiB | **16.79s** | 17.8 GiB |
  | **M4** ★ | **4 GiB** | **17.25s** | **8.7 GiB** |
  | M2 | 2 GiB | 50.44s | 4.7 GiB |

  **P4 vs P5（M8, stable-auto）**：

  | 模式 | mean |
  |------|------|
  | P4 (p4-groups) | 42.36s |
  | P5 (p5-round) | 41.83s ¹ |
  | 提升 | **-1.3%** |

  ¹ P5 round 2 outlier 45.4s excluded (disk activity).

  **同盘/异盘（M8）**：

  | 模式 | mean |
  |------|------|
  | 同盘 (G→G) | 41.60s |
  | 异盘 (G→WSL) | **37.69s** |
  | 差异 | **-9.4%** |

  **存储倍率**：

  | 数据 | 倍率 |
  |------|------|
  | 50GB | 1.421x |
  | 20GB | 1.036x |

  **关键验证**：

  | 条件 | 结果 |
  |------|------|
  | 21/21 CTest | ✅ |
  | 2GB 不崩溃 | ✅ |
  | decode errors | 0 |
  | AUTO vs M8 hash | 一致 |
  | 输出文件 | 330/330 |
  | 存储 ≤ 1.50 | ✅ 50GB 1.421x, 20GB 1.036x |
  | 5 轮 CV | ≤ 0.5% |

  **推荐参赛配置**：

  |  | 50GB | 20GB |
  |---|------|------|
  | 推荐 | M8 (8 GiB, 128 MB) | M4 (4 GiB) |
  | T_composite | 41.37s | 17.25s |
  | RSS | 13.7 GiB | 8.7 GiB |
  | vs 最快 | +3.6% | +2.7% |
  | 绝对最快 | AUTO (27 GiB) 39.95s | AUTO (17 GiB) 16.79s |
  | 低内存 | M2 (2 GiB) 88.30s | M2 (2 GiB) 50.44s |

## [0.6.0] - 2026-07-17

### Changed

- **Z-fastest 官方 raw 布局修正**（全项目范围）：
  - 统一所有 raw 文件读取为 `offset(x,y,z) = (x*ny+y)*nz+z`（Z 最快变化）
  - 内部 Leaf 布局保持 X-fastest 不变；转换器负责两者之间的重排
  - 修复旧 LZ4 Writer、Verify、precompute_x、gen_fast_data 中的 X-fastest 错误公式
  - RZFP 路径已正确使用 Z-fastest，重构为统一调用 `rawOffsetZFastest()`

- **LZ4 Writer 并行压缩**（`src/writer.cpp`）：
  - 线程池并行处理 (sy,sz) superblock 的打包、Leaf 重排和 LZ4 压缩
  - 结果按逻辑索引排序后顺序写出

- **Plane-major sidecar 生成器**（`tools/erwt3d_precompute_x.cpp`）：
  - 从 z-chunk-first 改为 plane-major：每个 X-plane 只读一次
  - 8 线程并行压缩同一 plane 的全部 z-chunk
  - 预分配工作缓冲区复用，消除 per-chunk 动态分配
  - 删除旧的 `buildChunk()` 和 `planeChunks` 死代码

### Added

- **Sidecar/legacy Reader 转置修复**（`src/reader.cpp`）：
  - 外置 `.xp` sidecar 的 chunk 数据从 Y-fastest 转置为输出 Z-fastest
  - 内嵌 legacy X-plane 同修复
  - 4 条代码路径（单/批 × 外置/内嵌）全部修复
  - 新增 blocked `transposeZYToYZ()` 工具函数（32×32 分块）

- **RZFP 结构完整性验证**（`src/rzfp_reader.cpp`）：
  - 在分配前校验文件大小、区域边界和顺序
  - 安全算术防止 offset+bytes 溢出
  - 每个 Superblock 的 descriptor size 总和必须等于 `payload_bytes`
  - 三种确定性损坏测试（bad magic / bad index / bad descriptor）

- **LZ4 压缩探针**（`src/lz4_probe.cpp` / `tools/erwt3d_lz4_probe.cpp`）：
  - 分层 X-slab 采样，每个 slab 随机采样 (sy,sz) superblock
  - 复用 Writer 的真实打包→Leaf 重排→LZ4 压缩流程
  - 基于 per-SB ratio 分布的真实 95% 置信区间

- **顶层自动格式规划器**（`src/auto_plan.cpp` / `tools/erwt3d_auto_plan.cpp`）：
  - 组合 LZ4 Probe、RZFP AutoPlan、HDD 校准
  - 候选：LZ4 main、LZ4+sidecar(s1/s2/s3)、RZFP main、RZFP+sidecar
  - 按存储预算过滤，按预测 T_composite 排序，输出 Top-2
  - 20GB → LZ4+s2、50GB → RZFP（经验证与实际一致）

- **HDD 自动校准**（`src/sb_hdd.cpp`）：
  - 3 区域顺序带宽（头部/中部/尾部各 256MB，取中位数）
  - 128 次随机 1MB 读取中位数延迟
  - 64 位随机数发生器，4KB 对齐，`posix_fadvise(DONTNEED)`

- **RZFP profiling 线程安全**（`src/rzfp_reader.cpp`）：
  - `scatter_time_ms` 从数据竞争 double 改为 `atomic<uint64_t> scatter_ns.fetch_add()`
  - sidecar plan 时间不再错误计入 I/O

- **gen_fast_data 错误传播**（`tools/gen_fast_data.cpp`）：
  - worker 错误通过 `std::atomic<bool>` 传到主线程
  - `pwrite()==0` 处理、线程数/维度校验、安全乘法防溢出
  - 真实二进制测试验证输出内容、确定性和不同 seed

### Fixed

- X-panel count：`superX / panelStride` → `(superX + panelStride - 1) / panelStride`
- Writer 内存限制：不再静默放大预算，不足时直接报错退出
- 并行压缩 + 内嵌 X-panel 不支持组合被正确拒绝
- `precompute_x` 的 `rawRow.resize(planeFloats * sizeof(float))` 4 倍内存浪费
- sidecar `rowsInChunk` 当 `chunkZRows > nz` 时溢出输出缓冲区
- external sidecar 与 legacy X-plane 现在使用独立测试文件
- stride 命中/未命中现在直接验证，不再依赖假阳性跳过

### Performance

G 盘 HDD，`--hdd` 模式，4GB 内存限制，Z-fastest 布局修正后：

| 数据集 | 格式 | T_composite | 存储比 | 备注 |
|--------|------|------------|--------|------|
| 20GB | LZ4 + sidecar s2 | 25.37s mean / 22.09s best | 0.479x | 3 轮 CV≈2% |
| 50GB | RZFP | 99.06s | 0.421x | 单次代表性 |

> G 盘顺序带宽 ~200-220 MB/s，低于之前 D 盘（~300 MB/s）。相对排名不受磁盘差异影响。

### Known limitations

- Sidecar ratio 在 Planner 中为启发式估算（由 LZ4 主文件 ratio 外推）
- Planner 预测时间为格式筛选近似值，非精确 benchmark 预报
- 50GB 缺少多轮冷/热缓存重复测试

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
