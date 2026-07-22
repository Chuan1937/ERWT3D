# ERWT3D Rocky Linux HDD 测试结果

## 测试环境

- **日期**: 2026-07-22
- **系统**: Rocky Linux 9.8, VMware VM, 16 vCPU (i9-10850K), ~62 GiB RAM
- **存储**: Windows G 盘 HDD, 通过 vmhgfs-fuse 挂载
- **分支**: `bench/cup-large-scale-first`
- **编译**: GCC 11.5.0, Release, RZFP enabled
- **线程**: 16
- **种子**: 20260511
- **数据格式**: RZFP (raw-x-aux auto)

## 注意

⚠️ FSTYPE=fuse.vmhgfs-fuse — 数据位于 Windows G 盘，通过 VMware 共享文件夹访问
⚠️ /home 也是 G 盘 (同一 HDD)，非独立 NVMe
⚠️ 测试结果反映 HDD + FUSE 叠加性能，不等同于原生 ext4 on HDD

## 5GB 测试结果 (929×1033×1399, RZFP 0.589x)

| 配置 | 内存限制 | Window Cache | IO Buffer | T_composite | Read time | Write time |
|------|---------|-------------|-----------|-------------|-----------|------------|
| AUTO | 9115 MiB | 2971 MiB | 1024 MiB | 14.849s | 87.024s | 2.068s |
| M2 | 2048 MiB | 952 MiB | 256 MiB | 6.513s | 36.888s | 2.192s |
| M4 | 4096 MiB | 2744 MiB | 512 MiB | 6.147s | 34.554s | 2.325s |
| M8 | 8192 MiB | 2971 MiB | 1024 MiB | 6.118s | 34.275s | 2.432s |
| M16 | 16384 MiB | 2971 MiB | 1024 MiB | 6.146s | 34.546s | 2.333s |
| M32 | 32768 MiB | 2971 MiB | 1024 MiB | 6.049s | 34.118s | 2.177s |

### 5GB 分析

- AUTO 异常慢 (14.849s)，Read time 87s 远高于其他配置 (~34s)
- M4~M32 性能接近 (6.0~6.5s)，M2 略慢
- AUTO 慢的原因：Device 检测为 32.1 MB/s (FUSE 小 IO 探测)，导致窗口策略过于保守
- M2~M32 Device 检测为 ~740 MB/s，窗口策略正常

## 10GB 测试结果 (1185×1289×1751, RZFP 0.588x)

| 配置 | 内存限制 | Window Cache | IO Buffer | T_composite | Read time | Write time |
|------|---------|-------------|-----------|-------------|-----------|------------|
| AUTO | 12057 MiB | 5913 MiB | 1024 MiB | 25.974s | 152.558s | 3.284s |
| M2 | 2048 MiB | 754 MiB | 256 MiB | 10.179s | 57.537s | 3.536s |
| M4 | 4096 MiB | 2435 MiB | 512 MiB | 9.736s | 54.544s | 3.875s |
| M8 | — | — | — | — | — | — |
| M16 | — | — | — | — | — | — |
| M32 | — | — | — | — | — | — |

> 10GB M8/M16/M32 待测试

## 20GB 测试结果 (1493×1621×2218, RZFP 0.588x)

| 配置 | 内存限制 | Window Cache | IO Buffer | T_composite | Read time | Write time |
|------|---------|-------------|-----------|-------------|-----------|------------|
| AUTO | — | — | — | — | — | — |
| M2 | — | — | — | — | — | — |
| M4 | — | — | — | — | — | — |
| M8 | — | — | — | — | — | — |
| M16 | — | — | — | — | — | — |
| M32 | — | — | — | — | — | — |

> 20GB 全部待测试

## 40GB / 70GB / 100GB

> 未测试。100GB 用户确认不需要。

## 关键发现

1. **AUTO 模式在 FUSE 下严重退化**：Device 探测测到极低带宽 (32 MB/s for 5GB, 21 MB/s for 10GB)，导致窗口策略异常保守，Read time 翻倍
2. **手动内存限制正常**：M2~M32 Device 探测 ~740 MB/s，性能符合预期
3. **AUTO 问题根因**：FUSE vmhgfs-fuse 对小 IO 探测 (用于测 Device 带宽) 性能极差，不代表实际大块 IO 带宽
4. **存储比全部达标**：0.588x~0.589x，远低于 1.5x 上限

## 与之前 WSL 测试对比

| 规模 | 配置 | WSL (9p) T_composite | Rocky (FUSE) T_composite | 变化 |
|------|------|---------------------|-------------------------|------|
| 5GB | AUTO | 7.665s | 14.849s | +93.6% |
| 10GB | AUTO | 11.773s | 25.974s | +120.6% |
| 20GB | AUTO | 11.677s | — | — |

> WSL 9p 和 Rocky FUSE 都是共享文件夹协议，但 FUSE 性能更差，尤其 AUTO 模式
