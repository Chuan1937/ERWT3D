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
- **缓存策略**: 每组测试前 `sync && echo 3 > /proc/sys/vm/drop_caches` 清除 page cache

## 注意

⚠️ FSTYPE=fuse.vmhgfs-fuse — 数据位于 Windows G 盘，通过 VMware 共享文件夹访问
⚠️ /home 也是 G 盘 (同一 HDD)，非独立 NVMe
⚠️ 测试结果反映 HDD + FUSE 叠加性能，不等同于原生 ext4 on HDD
⚠️ HDD I/O 波动较大，同配置多次运行 T_composite 可差 ±1s
⚠️ **冷缓存测试**：每组前清 page cache，模拟比赛首次运行

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

### 冷缓存 vs 热缓存

- 热缓存测试中，后跑的配置会沾前面测试的 page cache 光，结果偏快且不可复现
- 20GB AUTO 冷缓存 63.9s vs 热缓存 15.6s，差 4 倍——完全是 page cache 效应
- **比赛只跑一次，冷缓存才是真实成绩**，因此所有测试改为每组前清 page cache

## 5GB 测试结果 (929×1033×1399, RZFP 0.589x)

### 冷缓存（每组前 drop_caches）

| 配置 | 内存限制 | Window Cache | IO Buffer | T_composite | Read time | Write time |
|------|---------|-------------|-----------|-------------|-----------|------------|
| AUTO | 9115 MiB | 2971 MiB | 1024 MiB | 6.275s | 35.452s | 2.197s |
| M2 | 2048 MiB | 952 MiB | 256 MiB | 6.609s | 37.538s | 2.116s |
| M4 | 4096 MiB | 2744 MiB | 512 MiB | 6.309s | 35.779s | 2.077s |
| M8 | 8192 MiB | 2971 MiB | 1024 MiB | **6.226s** | 35.191s | 2.166s |
| M16 | 16384 MiB | 2971 MiB | 1024 MiB | 6.388s | 35.821s | 2.508s |
| M32 | 32768 MiB | 2971 MiB | 1024 MiB | 6.365s | 36.124s | 2.065s |

### 分析

- 所有配置性能一致：6.2~6.6s，差距仅 6%
- M2 最慢（6.609s）：window cache 仅 952 MiB，IO buffer 256 MiB
- M4~M32 的 window cache ≥2744 MiB，已足够缓存 5GB RZFP（仅 3GB）的热数据
- Read time 主体（35~37s），Write time 固定 ~2s
- **5GB 规模太小不具代表性**：RZFP 仅 3GB，内存不是瓶颈
- **推荐：AUTO**

### 内存使用

- AUTO: MemAvailable ~60 GiB, 实际使用 ~9 GiB
- M2: 实际使用 ~2 GiB，性能损失 ~6%

## 10GB 测试结果 (1185×1289×1751, RZFP 0.588x)

> 待冷缓存重测

## 20GB 测试结果 (1493×1621×2218, RZFP 0.588x)

> 待冷缓存重测

## 50GB 测试结果

> 待测试（需先生成 RZFP 文件）

## 关键发现

1. **Device 探测对比赛不利**：FUSE 下不稳定 + 耗时 2~3s，直接用 preset 更优
2. **冷缓存是比赛真实成绩**：热缓存测试结果不可复现，必须每组前清 page cache
3. **存储比全部达标**：0.588x~0.589x，远低于 1.5x 上限（存储满分 20 分）
4. **HDD I/O 是根本瓶颈**：同配置多次运行 T_composite 波动 ±1~2s
5. **小数据集内存不是瓶颈**：5GB 下 M2~M32 性能差距 <6%
6. **比赛计时含全部开销**：进程启动、内存分配、I/O 读取、解码、写出，都不能忽略
