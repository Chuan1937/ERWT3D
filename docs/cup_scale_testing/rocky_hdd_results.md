# ERWT3D Rocky Linux HDD 测试结果

## 测试环境

- **日期**: 2026-07-22
- **系统**: Rocky Linux 9.8, VMware VM, 16 vCPU (i9-10850K), ~62 GiB RAM, 31.4 GiB swap
- **存储**: Windows G 盘 HDD, 通过 vmhgfs-fuse 挂载（/mnt/g, /home 同一盘）
- **分支**: `bench/cup-large-scale-first`
- **编译**: GCC 11.5.0, Release, RZFP enabled, LZ4 enabled, -march=native
- **线程**: 8
- **种子**: 20260511
- **ZFP**: v1.0.1, LD_LIBRARY_PATH=/root/ERWT3D/deps/zfp/lib64
- **Device 配置**: 250 MB/s preset, seek 10ms (跳过运行时探测)
- **缓存策略**: 每组测试前 `sync && echo 3 > /proc/sys/vm/drop_caches` 清除 page cache
- **AUTO 内存**: 70% MemAvailable 硬上限，4 GiB reserve 保护
- **Read Window**: 最大 128 MiB（共享文件夹保守策略）

## 注意

⚠️ FSTYPE=fuse.vmhgfs-fuse — 数据位于 Windows G 盘，通过 VMware 共享文件夹访问
⚠️ /home 也是 G 盘 (同一 HDD)，非独立 NVMe
⚠️ 测试结果反映 HDD + FUSE 叠加性能，不等同于原生 ext4 on HDD
⚠️ HDD I/O 波动较大，同配置多次运行 T_composite 可差 ±1s
⚠️ **冷缓存测试**：每组前清 page cache，模拟比赛首次运行

## 真实赛题数据质量检查

| 文件 | 维度 | 大小 | 零值比 | Min | Max | Mean | Std |
|------|------|------|--------|-----|-----|------|-----|
| cup_3d_small.dat | 801×2405×2501 | 17.95 GiB | 11.15% | -120 | 120 | 0.40 | 24.12 |
| cup_3d_big.dat | 2001×2201×3000 | 49.22 GiB | 6.01% | -26875 | 26545 | 0.43 | 5485.02 |

- 文件大小与维度完全匹配 ✅
- 数据有真实变化，非全零 ✅
- big.dat 动态范围远大于 small.dat（std 5485 vs 24）

## 20GB 真实赛题数据对比 (801×2405×2501)

### 转换结果

| 格式 | 文件大小 | 存储比 | 转换时间 |
|------|---------|--------|---------|
| Raw | 17.95 GiB | 1.000x | — |
| LZ4 | 7.76 GiB | **0.432x** | 5m23s |
| RZFP | 10.89 GiB | **0.607x** | 8m13s |

### LZ4 冷缓存 3 次 (erwt3d_bench_contest, --hdd, 8 threads, 4096 MiB)

| Run | X random | Y random | Z random | X cont | Y cont | Z cont | Total | T_composite |
|-----|----------|----------|----------|--------|--------|--------|-------|-------------|
| 1 | 18.49s | 8.75s | 7.81s | 1.09s | 0.31s | 0.32s | 36.77s | **6.128s** |
| 2 | 18.66s | 8.79s | 7.82s | 1.10s | 0.31s | 0.33s | 37.01s | **6.168s** |
| 3 | 18.67s | 8.83s | 7.72s | 1.11s | 0.30s | 0.32s | 36.94s | **6.157s** |

**LZ4 中位数 T_composite = 6.157s**

### RZFP 冷缓存 3 次 (erwt3d_contest, AUTO 70%, 8 threads)

| Run | Setup | Prepare | Read | Write | Close | Total (e2e) | T_composite | Process e2e |
|-----|-------|---------|------|-------|-------|-------------|-------------|-------------|
| 1 | 0.000s | 6.322s | 76.761s | 5.762s | 0.001s | 90.351s | **15.059s** | 90.703s |
| 2 | 0.000s | 6.934s | 76.957s | 6.151s | 0.001s | 91.601s | **15.267s** | 92.053s |
| 3 | 0.000s | 7.329s | 76.925s | 5.849s | 0.001s | 91.639s | **15.273s** | 91.989s |

**RZFP 中位数 T_composite = 15.267s**

RZFP AUTO 配置：MemAvailable ~60 GiB, hard limit ~42 GiB, window cache 10995 MiB, read window 128 MiB

### 20GB 对比结论

| 指标 | LZ4 | RZFP | LZ4 优势 |
|------|-----|------|---------|
| T_composite (中位数) | **6.157s** | 15.267s | **2.48x** |
| 存储比 | **0.432x** | 0.607x | 文件小 29% |
| 文件大小 | 7.76 GiB | 10.89 GiB | 少 3.13 GiB |
| Read time | ~29s (6组) | ~77s (6组) | **2.66x** |
| Write time | ~6s (6组) | ~6s (6组) | 持平 |

**LZ4 在 20GB 真实赛题数据上碾压 RZFP**：
1. LZ4 压缩率 0.432x 远优于 RZFP 0.607x，文件小 3.1 GiB，I/O 量少 29%
2. LZ4 解码速度远快于 ZFP 解码（LZ4 解码 ~4 GB/s vs ZFP 解码 ~1 GB/s）
3. Read time LZ4 ~29s vs RZFP ~77s，差 2.66 倍
4. 两者存储比均远低于 1.5x 上限，存储分都是满分 20/20

## 代码变更 (本次提交)

### 1. AUTO 内存预算改为 70% MemAvailable

`src/memory_budget.cpp`:
- 旧：`reserve = max(2 GiB, MemAvailable/8)`, `total = min(safeProcessRss, payload+6GiB)`
- 新：`autoLimit = MemAvailable × 70%`, `safeLimit = min(autoLimit, MemAvailable - 4GiB)`, `total = safeLimit`
- 70% 是硬上限，不要求程序必须分配完
- 4 GiB reserve 保护，防止小内存系统 OOM

### 2. Read Window 限制最大 128 MiB

`tools/erwt3d_contest.cpp`:
- 旧：auto 时 `min(512 MiB, window_cache/2)`, floor 128 MiB
- 新：auto 时 `min(128 MiB, window_cache/2)`, floor 64 MiB
- 显式 `--read-window-mb` 也被 cap 到 128 MiB
- 共享文件夹环境保守策略，避免大窗口导致 HDD 抖动

### 3. 端到端计时

`src/contest_round_executor.cpp`:
- 旧：`total_time_ms = readMs + writeMs`（不含 setup/prepare/close）
- 新：`total_time_ms = wallMs`（含全部开销）

`tools/erwt3d_contest.cpp`:
- 新增 e2e 计时（从 main 开始到结束）
- 输出所有阶段耗时：Setup/Prepare/Read/Write/Close/Total/T_composite/Process e2e
- CSV 记录全部指标：内存配置、各阶段耗时、策略选择、leaf 统计

### 4. 保留命令行参数

- `--memory-limit-mb auto|N`：默认 auto（70% MemAvailable），题目要求保留
- `--read-window-mb N`：0=auto（max 128 MiB），显式值也被 cap
- `--threads N`：默认 8

## 关键发现

1. **LZ4 在真实赛题数据上远优于 RZFP**：20GB small.dat LZ4 T_composite 6.16s vs RZFP 15.27s，差 2.5 倍
2. **压缩率是关键**：LZ4 0.432x vs RZFP 0.607x，文件更小 = I/O 更少 = 更快
3. **LZ4 解码速度优势**：LZ4 解码 ~4 GB/s，ZFP 解码 ~1 GB/s，3-4 倍差距
4. **AUTO 70% MemAvailable 工作正常**：冷缓存下 MemAvailable ~60 GiB，hard limit ~42 GiB，不再退化
5. **Read Window 128 MiB 保守策略有效**：避免大窗口在 FUSE 环境下导致 HDD 抖动
6. **端到端计时更真实**：包含 setup/prepare/close，RZFP 的 prepare（文件预创建）占 6-7s
7. **Device 探测 preset 仍是最优**：250 MB/s preset，跳过运行时探测
8. **冷缓存是比赛真实成绩**：热缓存不可复现

## 待完成

1. **50GB big.dat 测试**：需转换 LZ4 和 RZFP，各跑 3 次
2. **50GB 格式选择**：big.dat std=5485（高动态范围），LZ4 压缩率可能不如 small.dat，需实测
3. **最终参赛格式决定**：根据 20GB+50GB 综合结果选择 LZ4 或 RZFP
