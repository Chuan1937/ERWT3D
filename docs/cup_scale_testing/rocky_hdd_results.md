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

## 注意

⚠️ FSTYPE=fuse.vmhgfs-fuse — 数据位于 Windows G 盘，通过 VMware 共享文件夹访问
⚠️ /home 也是 G 盘 (同一 HDD)，非独立 NVMe
⚠️ 测试结果反映 HDD + FUSE 叠加性能，不等同于原生 ext4 on HDD
⚠️ HDD I/O 波动较大，同配置多次运行 T_composite 可差 ±1s

## Device 探测修复

### 问题

FUSE vmhgfs-fuse 下 Device 探测极不稳定：
- 冷缓存时探测到 32 MB/s（5GB）或 21 MB/s（10GB）
- 热缓存时探测到 730+ MB/s
- 低带宽 → `applyFullscanProtection` 禁止 >8GB full scan → 选择 SelectiveLeaf（大量小 pread）→ FUSE 上更慢 → 恶性循环
- AUTO 模式 Read time 翻倍（87s vs 34s for 5GB）

### 修复 (commit 待定)

1. **预热读**：`DeviceCalibrationConfig::warmup_count = 1`，正式测量前做一次 discard 的预热读，消除 FUSE 冷启动
2. **带宽下限**：`DeviceCalibrationConfig::minimum_sequential_mb_s = 80.0`，探测结果低于 80 MB/s 时 clamp 到 80 MB/s（真正 HDD 顺序读不可能低于此值）
3. 所有 fallback 默认值从硬编码 80.0 改为读取 config

### 修复效果

| 配置 | 修复前 T_composite | 修复后 T_composite | Device 探测 |
|------|-------------------|-------------------|------------|
| AUTO (5GB) | 14.849s | 6.096s | 32→738 MB/s |
| AUTO (5GB, 第2次) | 7.352s | — | 736 MB/s (偶然正常) |

修复后 AUTO 稳定探测 ~730 MB/s，T_composite 与手动模式一致。

## 5GB 测试结果 (929×1033×1399, RZFP 0.589x)

### 修复前（Device 探测 bug）

| 配置 | 内存限制 | Window Cache | IO Buffer | T_composite | Read time | Write time | Device |
|------|---------|-------------|-----------|-------------|-----------|------------|--------|
| AUTO | 9115 MiB | 2971 MiB | 1024 MiB | 14.849s | 87.024s | 2.068s | 32.1 MB/s |
| M2 | 2048 MiB | 952 MiB | 256 MiB | 6.513s | 36.888s | 2.192s | 715.5 MB/s |
| M4 | 4096 MiB | 2744 MiB | 512 MiB | 6.147s | 34.554s | 2.325s | 772.4 MB/s |
| M8 | 8192 MiB | 2971 MiB | 1024 MiB | 6.118s | 34.275s | 2.432s | 735.0 MB/s |
| M16 | 16384 MiB | 2971 MiB | 1024 MiB | 6.146s | 34.546s | 2.333s | 761.7 MB/s |
| M32 | 32768 MiB | 2971 MiB | 1024 MiB | 6.049s | 34.118s | 2.177s | 748.2 MB/s |

### 修复后（Device 探测稳定）

| 配置 | 内存限制 | Window Cache | IO Buffer | T_composite | Read time | Write time | Device |
|------|---------|-------------|-----------|-------------|-----------|------------|--------|
| AUTO | 9115 MiB | 2971 MiB | 1024 MiB | 6.096s | — | — | 738.7 MB/s |
| AUTO (第2次) | 9115 MiB | 2971 MiB | 1024 MiB | 7.061s | 40.130s | 2.235s | 738.7 MB/s |

### 5GB 分析

- 修复后 AUTO 与手动模式性能一致（6~7s），差异来自 HDD I/O 波动
- M4~M32 性能接近 (6.0~6.5s)，M2 略慢 (6.5s)，因为 window cache 小导致更多 cache miss
- 5GB 数据量小，window cache 2971 MiB 已足够缓存大部分热数据，内存限制对性能影响不大
- **推荐配置：AUTO**（修复后与手动模式无差异，无需手动调参）

### 内存使用

- AUTO: MemAvailable ~60 GiB, auto reserve ~7.5 GiB, 实际使用 ~9 GiB
- M2: 实际使用 ~2 GiB，性能损失 ~8%
- 5GB 规模下内存不是瓶颈，2 GiB 即可运行

## 10GB 测试结果 (1185×1289×1751, RZFP 0.588x)

### 修复前（Device 探测 bug）

| 配置 | 内存限制 | Window Cache | IO Buffer | T_composite | Read time | Write time | Device |
|------|---------|-------------|-----------|-------------|-----------|------------|--------|
| AUTO | 12057 MiB | 5913 MiB | 1024 MiB | 25.974s | 152.558s | 3.284s | 21.2 MB/s |
| M2 | 2048 MiB | 754 MiB | 256 MiB | 10.179s | 57.537s | 3.536s | 736.4 MB/s |
| M4 | 4096 MiB | 2435 MiB | 512 MiB | 9.736s | 54.544s | 3.875s | 748.9 MB/s |

> 10GB M8/M16/M32 待修复后重测

## 20GB 测试结果 (1493×1621×2218, RZFP 0.588x)

> 待测试

## 40GB / 70GB

> 待测试

## 100GB

> 用户确认不需要

## 关键发现

1. **Device 探测是 AUTO 模式的关键**：FUSE 下冷缓存探测极低 (21~32 MB/s)，导致策略选错，Read time 翻倍
2. **修复后 AUTO = 手动模式**：预热读 + 带宽下限 80 MB/s，探测稳定 ~730 MB/s
3. **存储比全部达标**：0.588x~0.589x，远低于 1.5x 上限（存储满分 20 分）
4. **HDD I/O 是根本瓶颈**：同配置多次运行 T_composite 波动 ±1s
5. **内存限制对小数据集影响小**：5GB 下 M2~M32 性能差距 <8%

## 与之前 WSL 测试对比

| 规模 | 配置 | WSL (9p) T_composite | Rocky FUSE (修复前) | Rocky FUSE (修复后) |
|------|------|---------------------|---------------------|---------------------|
| 5GB | AUTO | 7.665s | 14.849s | 6.096s |
| 10GB | AUTO | 11.773s | 25.974s | — |
| 20GB | AUTO | 11.677s | — | — |

> 修复后 Rocky FUSE 性能反而优于 WSL 9p，可能因为 FUSE 大块 IO 效率更高
