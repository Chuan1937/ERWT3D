# ERWT3D Rocky Linux HDD 测试结果

## 测试环境

- **日期**: 2026-07-22
- **系统**: Rocky Linux 9.8, VMware VM, 16 vCPU (i9-10850K), ~62 GiB RAM, 31.4 GiB swap
- **存储**: Windows G 盘 HDD, 通过 vmhgfs-fuse 挂载（/mnt/g, /home 同一盘）
- **分支**: `bench/cup-large-scale-first`
- **编译**: GCC 11.5.0, Release, RZFP enabled, -march=native
- **线程**: 16
- **种子**: 20260511
- **数据格式**: RZFP (raw-x-aux auto)
- **ZFP**: v1.0.1, LD_LIBRARY_PATH=/root/ERWT3D/deps/zfp/lib64
- **Device 配置**: 250 MB/s preset, seek 10ms (跳过运行时探测)

## 注意

⚠️ FSTYPE=fuse.vmhgfs-fuse — 数据位于 Windows G 盘，通过 VMware 共享文件夹访问
⚠️ /home 也是 G 盘 (同一 HDD)，非独立 NVMe
⚠️ 测试结果反映 HDD + FUSE 叠加性能，不等同于原生 ext4 on HDD
⚠️ HDD I/O 波动较大，同配置多次运行 T_composite 可差 ±1s

## Device 探测优化

### 问题

FUSE vmhgfs-fuse 下 Device 探测存在两个问题：

1. **不稳定**：冷缓存时探测到 21~32 MB/s，热缓存时 730+ MB/s
2. **耗时**：3次×128MB 顺序读 + 预热 = ~2.5s 额外开销，比赛计时从命令启动开始算

低带宽探测导致 `applyFullscanProtection` 禁止 >8GB full scan，策略选择 SelectiveLeaf（大量小 pread），Read time 翻倍。

### 修复

1. `DeviceCalibrationConfig` 增加 `warmup_count` 和 `minimum_sequential_mb_s` 参数
2. **contest 入口直接跳过 Device 探测**，使用 preset：`sequential_mb_s=250, seek_ms=10`
3. 消除 ~2.5s 探测开销 + FUSE 探测不稳定问题

## 5GB 测试结果 (929×1033×1399, RZFP 0.589x)

| 配置 | 内存限制 | Window Cache | IO Buffer | T_composite | Read time | Write time |
|------|---------|-------------|-----------|-------------|-----------|------------|
| AUTO | 9115 MiB | 2971 MiB | 1024 MiB | 7.108s | 40.527s | 2.0s |
| M2 | 2048 MiB | 952 MiB | 256 MiB | 6.513s | 36.888s | 2.192s |
| M4 | 4096 MiB | 2744 MiB | 512 MiB | 6.147s | 34.554s | 2.325s |
| M8 | 8192 MiB | 2971 MiB | 1024 MiB | 6.118s | 34.275s | 2.432s |
| M16 | 16384 MiB | 2971 MiB | 1024 MiB | 6.146s | 34.546s | 2.333s |
| M32 | 32768 MiB | 2971 MiB | 1024 MiB | 6.049s | 34.118s | 2.177s |

### 5GB 分析

- M4~M32 性能接近 (6.0~7.1s)，AUTO 与手动模式差异在 HDD 波动范围内
- M2 略慢 (6.5s)，window cache 952 MiB 偏小
- 5GB 数据量小 (RZFP 仅 3GB)，内存不是瓶颈
- **推荐：AUTO**

### 内存使用

- AUTO: MemAvailable ~60 GiB, 实际使用 ~9 GiB
- M2: 实际使用 ~2 GiB，性能损失 ~8%

## 10GB 测试结果 (1185×1289×1751, RZFP 0.588x)

| 配置 | 内存限制 | Window Cache | IO Buffer | T_composite | Read time | Write time |
|------|---------|-------------|-----------|-------------|-----------|------------|
| AUTO | 12057 MiB | 5913 MiB | 1024 MiB | 12.925s | 74.196s | 3.356s |
| M2 | 2048 MiB | 754 MiB | 256 MiB | 10.395s | 58.691s | 3.677s |
| M4 | 4096 MiB | 2435 MiB | 512 MiB | 9.926s | 56.300s | 3.256s |
| M8 | 8192 MiB | 5913 MiB | 1024 MiB | 12.319s | 70.747s | 3.165s |
| M16 | 16384 MiB | 5913 MiB | 1024 MiB | 10.411s | 58.971s | 3.493s |
| M32 | 32768 MiB | 5913 MiB | 1024 MiB | 10.718s | 61.020s | 3.285s |

### 10GB 分析

- AUTO (12.925s) 和 M8 (12.319s) 偏慢，M4 (9.926s) 最快
- AUTO/M8/M16/M32 的 window cache 都是 5913 MiB，但 Read time 差异大（58~74s），可能是 HDD I/O 波动
- M2/M4 window cache 小反而快，可能因为小 cache → 小窗口 → 更少的随机读取
- **10GB 规模下 HDD I/O 波动掩盖了内存参数的影响，难以得出确定结论**

### 内存使用

- AUTO: MemAvailable ~60 GiB, 实际使用 ~12 GiB
- M2: 实际使用 ~2 GiB，可运行但 Read time 略高

## 20GB 测试结果 (1493×1621×2218, RZFP 0.588x)

| 配置 | 内存限制 | Window Cache | IO Buffer | T_composite | Read time | Write time |
|------|---------|-------------|-----------|-------------|-----------|------------|
| AUTO | 18006 MiB | 11862 MiB | 1024 MiB | **63.896s** | 377.455s | 5.918s |
| M2 | 2048 MiB | 763 MiB | 256 MiB | 18.380s | 105.053s | 5.230s |
| M4 | 4096 MiB | 1924 MiB | 512 MiB | 15.394s | 86.756s | 5.605s |
| M8 | 8192 MiB | 5508 MiB | 1024 MiB | 15.643s | 88.680s | 5.175s |
| M16 | 16384 MiB | 11862 MiB | 1024 MiB | 15.426s | 87.588s | 4.966s |
| M32 | 32768 MiB | 11862 MiB | 1024 MiB | 15.583s | 88.328s | 5.170s |

### 20GB 分析

- **AUTO 严重异常**：T_composite=63.9s，Read time=377s，是手动模式的 4 倍
- AUTO 的 window cache (11862 MiB) 和 M16/M32 一样，但 Read time 差 4 倍
- M4~M32 性能接近 (15.4~15.6s)，M2 略慢 (18.4s)
- **AUTO 异常原因待查**：可能是 AUTO 模式下策略选择错误（250 MB/s preset + 大 window cache 导致选了 FullPayloadScan？）
- **推荐：M4~M16**（15.4s），避免 AUTO

### 内存使用

- AUTO: 实际使用 ~18 GiB
- M4: 实际使用 ~4 GiB，性能最优
- M2: 实际使用 ~2 GiB，可运行但慢 19%

## 40GB / 70GB

> 待测试

## 100GB

> 用户确认不需要

## 关键发现

1. **Device 探测对比赛不利**：FUSE 下不稳定 + 耗时 2~3s，直接用 preset 更优
2. **存储比全部达标**：0.588x~0.589x，远低于 1.5x 上限（存储满分 20 分）
3. **HDD I/O 是根本瓶颈**：同配置多次运行 T_composite 波动 ±1~2s
4. **20GB AUTO 严重退化**：T_composite 63.9s vs 手动 15.4s，策略选择错误，需排查
5. **比赛计时含全部开销**：进程启动、内存分配、I/O 读取、解码、写出，都不能忽略
