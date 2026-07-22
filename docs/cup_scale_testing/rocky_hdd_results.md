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

### 冷缓存（每组前 drop_caches）

| 配置 | 内存限制 | Window Cache | IO Buffer | T_composite | Read time | Write time |
|------|---------|-------------|-----------|-------------|-----------|------------|
| AUTO | 12057 MiB | 5913 MiB | 1024 MiB | **31.404s** | 185.160s | 3.265s |
| M2 | 2048 MiB | 754 MiB | 256 MiB | 10.257s | 58.345s | 3.195s |
| M4 | 4096 MiB | 2435 MiB | 512 MiB | 9.467s | 53.613s | 3.188s |
| M8 | 8192 MiB | 5913 MiB | 1024 MiB | **9.283s** | 52.436s | 3.259s |
| M16 | 16384 MiB | 5913 MiB | 1024 MiB | 9.269s | 52.280s | 3.335s |
| M32 | 32768 MiB | 5913 MiB | 1024 MiB | 9.350s | 52.917s | 3.186s |

### 分析

- **AUTO 冷缓存严重退化**：T_composite=31.4s，Read time=185s，是手动模式的 3.4 倍
- M4~M32 性能接近（9.3~9.5s），M2 略慢（10.3s）
- M8 最快（9.283s），M4~M32 差异在 HDD 波动范围内
- 10GB RZFP 约 6GB，M4（window cache 2435 MiB）已足够
- **推荐：M4~M8**（9.3~9.5s），避免 AUTO

### 内存使用

- AUTO: 实际使用 ~12 GiB（但冷缓存下性能差）
- M4: 实际使用 ~4 GiB，性能最优
- M2: 实际使用 ~2 GiB，性能损失 ~10%

## 20GB 测试结果 (1493×1621×2218, RZFP 0.588x)

### 冷缓存（每组前 drop_caches）

| 配置 | 内存限制 | Window Cache | IO Buffer | T_composite | Read time | Write time |
|------|---------|-------------|-----------|-------------|-----------|------------|
| AUTO | 18006 MiB | 11862 MiB | 1024 MiB | **61.600s** | 363.596s | 6.002s |
| M2 | 2048 MiB | 763 MiB | 256 MiB | 19.259s | 109.910s | 5.644s |
| M4 | 4096 MiB | 1924 MiB | 512 MiB | 16.481s | 92.842s | 6.044s |
| M8 | 8192 MiB | 5508 MiB | 1024 MiB | 15.110s | 85.388s | 5.271s |
| M16 | 16384 MiB | 11862 MiB | 1024 MiB | **15.100s** | 85.367s | 5.233s |
| M32 | 32768 MiB | 11862 MiB | 1024 MiB | 15.055s | 84.893s | 5.436s |

### 分析

- **AUTO 冷缓存严重退化**：T_composite=61.6s，Read time=364s，是手动模式的 4 倍
- M8~M32 性能接近（15.0~15.1s），M4 略慢（16.5s），M2 明显慢（19.3s）
- 20GB RZFP 约 12GB，M8（window cache 5508 MiB）开始够用
- M2 window cache 仅 763 MiB，远不够缓存 12GB 数据，Read time 110s
- **推荐：M8~M16**（15.1s），M2 不可用

### 内存使用

- AUTO: 实际使用 ~18 GiB（但冷缓存下性能差）
- M8: 实际使用 ~8 GiB，性能最优
- M4: 实际使用 ~4 GiB，慢 9%
- M2: 实际使用 ~2 GiB，慢 28%

## 50GB 测试结果 (2001×2201×3000, RZFP 1.008x with raw-x-aux)

### 冷缓存（每组前 drop_caches）

| 配置 | 内存限制 | Window Cache | IO Buffer | T_composite | Read time | Write time |
|------|---------|-------------|-----------|-------------|-----------|------------|
| AUTO | 6144 MiB | 0 MiB | 768 MiB | **19.677s** | 108.067s | 9.995s |
| M2 | 2048 MiB | 0 MiB | 256 MiB | **10.337s** | 52.065s | 9.957s |
| M4 | 4096 MiB | 0 MiB | 512 MiB | 11.481s | 59.691s | 9.196s |
| M8 | 8192 MiB | 0 MiB | 1024 MiB | 11.319s | 58.717s | 9.195s |
| M16 | 16384 MiB | 0 MiB | 1024 MiB | 11.324s | 58.730s | 9.215s |
| M32 | 32768 MiB | 0 MiB | 1024 MiB | 11.480s | 59.078s | 9.802s |

### 分析

- **所有配置 window cache = 0 MiB**：50GB RZFP 数据几乎全是 zero leaves（215610200/215613440），payload 仅 829KB，所以 window cache 无意义
- **M2 最快**（10.337s）：IO buffer 最小（256 MiB），每次读少，HDD 顺序读效率高
- **M4~M32 性能接近**（11.3~11.5s）：IO buffer 512~1024 MiB，差异在 HDD 波动范围内
- **AUTO 又退化**（19.677s）：MemAvailable 在清缓存后偏低（buff/cache 被清），auto reserve 计算 导致内存预算过小，window cache=0 + IO buffer=768MiB
- 50GB 规模下 Write time 固定 ~9.5s，是 Read time 的 ~17%
- **推荐：M2**（10.3s），因为 window cache 无用，IO buffer 越小 HDD 越顺序

### 数据特征

- raw 50GB，RZFP payload 仅 829KB（数据几乎全零）
- raw-x-aux 50GB（追加），总存储比 1.008x
- 这是 gen_fast_data 生成的数据，**不是真实赛题数据**，压缩率不具代表性

### 内存使用

- M2: 实际使用 ~2 GiB，性能最优
- AUTO: 实际使用 ~6 GiB，但性能差（冷缓存下 auto 预算不足）

## 汇总

| 规模 | 维度 | 存储比 | 最快配置 | 最快 T_composite | AUTO T_composite | AUTO 退化倍数 |
|------|------|--------|---------|-----------------|-----------------|-------------|
| 5GB | 929×1033×1399 | 0.589x | M8 | 6.226s | 6.275s | 1.0x |
| 10GB | 1185×1289×1751 | 0.588x | M16 | 9.269s | 31.404s | 3.4x |
| 20GB | 1493×1621×2218 | 0.588x | M32 | 15.055s | 61.600s | 4.1x |
| 50GB | 2001×2201×3000 | 1.008x | M2 | 10.337s | 19.677s | 1.9x |

### 推荐参赛配置

| 数据规模 | 推荐内存 | 预期 T_composite |
|---------|---------|-----------------|
| ≤5GB | AUTO 或 M4 | ~6s |
| ~10GB | M4~M8 | ~9.5s |
| ~20GB | M8~M16 | ~15s |
| ~50GB | M2~M4 | ~10~11s |

> 50GB 因测试数据几乎全零不具代表性，真实赛题数据 RZFP 压缩率应接近 0.588x（参考 5/10/20GB）

## 关键发现

1. **Device 探测对比赛不利**：FUSE 下不稳定 + 耗时 2~3s，直接用 preset 更优
2. **冷缓存是比赛真实成绩**：热缓存测试结果不可复现，必须每组前清 page cache
3. **AUTO 冷缓存退化**：10GB/20GB/50GB 下 AUTO 冷缓存均比手动慢 2~4 倍，原因是 `drop_caches` 后 MemAvailable 偏低，auto 预算计算不足
4. **存储比全部达标**：5/10/20GB RZFP 0.588x，50GB RZFP+raw-x-aux 1.008x，均远低于 1.5x 上限
5. **HDD I/O 是根本瓶颈**：同配置多次运行 T_composite 波动 ±1~2s
6. **内存断崖**：每个规模存在最低安全内存，低于此值 Read time 急剧增加
   - 5GB: 2 GiB 安全
   - 10GB: 4 GiB 安全
   - 20GB: 8 GiB 安全
   - 50GB: 2 GiB 即可（数据几乎全零）
7. **50GB 测试数据不具代表性**：gen_fast_data 生成数据几乎全零，RZFP 压缩后仅 829KB payload，真实赛题数据压缩率应更低
8. **比赛计时含全部开销**：进程启动、内存分配、I/O 读取、解码、写出，都不能忽略

## 待解决问题

1. **AUTO 冷缓存退化**：需要修复 auto 内存预算在 `drop_caches` 后的计算逻辑，或比赛时强制使用手动内存限制
2. **50GB 需用真实赛题数据重测**：当前 gen_fast_data 生成的数据几乎全零，RZFP payload 仅 829KB，不反映真实 I/O 压力
3. **RZFP vs LZ4 选择**：5/10/20GB 用 RZFP，50GB 也用 RZFP+raw-x-aux，需确认真实赛题数据下哪种格式更优
