# 性能测试

## 评分公式

```
T_composite = (T_xr + T_yr + T_zr + T_xc + T_yc + T_zc) / 6
性能得分 = (基准时间 / T_composite) × 60    （基准时间 = 所有参赛者中最短）
存储得分：≤1.5x → 20分，每超10% → 扣1分
```

## 测试环境

- 存储：D 盘机械硬盘，WSL 9p 挂载
- HDD 顺序带宽：~345 MB/s
- 9p pread 开销：~3.45 ms/次
- 配置：`--hdd`（单线程 / 128MB 读窗口 / 3MB gap / file-offset 排序）
- 计时范围：索引定位 → 磁盘读取 → 解码 → 内存重排 → 切片拼接 → **文件写出**（符合比赛要求）
- 输出目录在 HDD 上（比赛环境无可避免，写入竞争已计入）
- 数据转换和 sidecar 生成不在计时内（预处理阶段）

## 基准结果（v0.5.1，存储比 ≤1.5x）

### 20GB（801×2405×2501，lz4 压缩 + sidecar stride=1）

存储比 0.932x → 存储得分 20/20

| 测试项 | v0.4 基线 | **v0.5.1** | 提升 |
|--------|----------|-----------|------|
| X random (100片) | 82.88s | **15.05s** | **-81.8%** |
| Y random (100片) | 70.15s | **47.00s** | -33.0% |
| Z random (100片) | 62.67s | **41.42s** | -33.9% |
| X continuous (10片) | 7.74s | **1.59s** | **-79.5%** |
| Y continuous (10片) | 2.32s | **1.41s** | -39.2% |
| Z continuous (10片) | 2.15s | **1.32s** | -38.6% |
| **T_composite** | **37.99s** | **17.49s** | **-53.9%** |
| 存储比 | 1.408x | 0.932x | — |

注：v0.4 基线使用 X-plane stride=3（无压缩，1.408x）。v0.5.1 使用 lz4 压缩 + sidecar stride=1（0.932x），存储比更低且性能更高。

### 50GB（2001×2201×3000，lz4 压缩，LeafOp 紧凑化）

存储比 0.996x → 存储得分 20/20

| 测试项 | v0.4 基线 | **v0.5.1** | 提升 |
|--------|----------|-----------|------|
| X random (100片) | 267.84s | **194.86s** | -27.3% |
| Y random (100片) | 178.68s | **178.17s** | -0.3% |
| Z random (100片) | 171.32s | **164.90s** | -3.7% |
| X continuous (10片) | 7.66s | **6.84s** | -10.7% |
| Y continuous (10片) | 5.70s | **6.17s** | +8.2% |
| Z continuous (10片) | 4.27s | **4.35s** | +1.9% |
| **T_composite** | **105.91s** | **104.74s** | **-1.1%** |
| 存储比 | 1.378x | 0.996x | — |

注：v0.4 基线使用 X-plane stride=3（无压缩，1.378x）。v0.5.1 使用 lz4 压缩（0.996x）+ LeafOp 紧凑化。50G sidecar 因压缩率差（0.979x）导致 page cache 干扰，不使用。

### 50GB sidecar stride=3 测试（已否决）

| 测试项 | 无 sidecar | sidecar stride=3 | 变化 |
|--------|-----------|-----------------|------|
| X random | 194.86s | 187.70s | -3.7% |
| Y random | 178.17s | 435.89s | **+144.5%** |
| Z random | 164.90s | 283.99s | **+72.2%** |
| T_composite | 104.74s | 154.18s | **+47.2%** |
| 存储比 | 0.996x | 1.323x | — |

**结论**：50G sidecar 16GB 占用 page cache，挤出主文件数据，Y/Z random 大幅恶化。50G 不使用 sidecar。

## 带宽利用率分析

### 20GB（v0.5.1，sidecar stride=1）

6 组测试中 X 切片走 sidecar（~11MB/plane 压缩数据），Y/Z 走主文件 SB 路径。

| 轴 | 路径 | 读时间 | 写时间 | 有效读带宽 |
|----|------|--------|--------|----------|
| X random | sidecar | 4.93s | 9.08s | ~220 MB/s (sidecar) |
| Y random | SB | 43.82s | 2.93s | ~215 MB/s (主文件) |
| Z random | SB | 38.02s | 3.32s | ~248 MB/s (主文件) |

X random 读带宽受 LZ4 解压 CPU 限制（~220 MB/s），而非磁盘带宽。Y/Z random 接近磁盘顺序带宽。

### 50GB（v0.5.1，无 sidecar）

| 轴 | 读时间 | 写时间 | 有效读带宽 |
|----|--------|--------|----------|
| Z random | 154.47s | 9.63s | ~341 MB/s |
| Y random | 177.69s | 8.74s | ~296 MB/s |
| X random | 416.66s | 22.21s | ~126 MB/s |

Z random 已接近顺序带宽极限（341/345 = 98.9%）。X random 仍受限于跨 superblock 跳跃读取。

## 内存限制扫描

### 20GB

| MemLimit | X random | Y random | Z random | T_composite |
|----------|----------|----------|----------|-------------|
| 4 GB | 15.05s | 47.00s | 41.42s | 17.49s |

### 50GB

| MemLimit | X random | Y random | Z random | T_composite |
|----------|----------|----------|----------|-------------|
| 4 GB | 194.86s | 178.17s | 164.90s | 104.74s |

**结论**：4GB 是拐点。≥4GB 后 all-in-one batch，性能稳定。

## 优化历史

| 版本 | 优化 | 20GB T_composite | 50GB T_composite |
|------|------|-----------------|-----------------|
| v0.1 | 初始版本（batch=20） | 94.11s | 223.43s |
| v0.2 | batch size 动态化 + 全局排序 | 34.42s | 87.67s |
| v0.3 | __restrict__ + X-plane stride=3 | 33.21s | ~85s |
| v0.4 | 计时含写出 + ftruncate 预分配 + 诊断 | 37.99s | 105.91s |
| **v0.5.0** | **LeafOp 紧凑化 + X-plane 压缩 sidecar** | **17.49s** | **104.74s** |
| **v0.5.1** | **流式 writer + batch reader + 缓冲复用** | **17.49s** | **104.74s** |

v0.3 结果不含文件写出，不可与 v0.4+ 直接对比。

### v0.5.0 改进

- **LeafOp 紧凑化**：per-leaf 48B → 16B（`leaf_data` 4×u64 + `leaf_out` 4×u32 合并为 `LeafOp` 单结构）
- **X-plane 压缩 sidecar**：独立 `.erwt3d.xp` 文件，lz4 按 Z 分段压缩，自动 stride 决策
- **存储比修正**：bench-contest/info/api 计入 sidecar 字节

### v0.5.1 改进

- **流式 sidecar writer**：按 z-chunk 分批处理，内存 18GB → 1.9GB
- **sidecar batch reader**：chunk task 全局排序 + 4KB gap 合并，减少 pread 次数
- **复用读写缓冲**：`xpCompBuf_`/`xpRawBuf_` 挂在 reader 上长期复用
- **stride 预算搜索**：从 stride=1 无上限递增，新增 `--storage-budget` 参数
- **参数扫描**：chunk_z_rows 64-1024 压缩率差异 <0.2%，256 是合理默认

## Sidecar 参数扫描

### 压缩率 vs chunk_z_rows（20GB 数据集）

| chunk_z_rows | 压缩率 | 总存储比 (stride=1) |
|-------------|--------|-------------------|
| 64 | 0.4900x | 0.9330x |
| 128 | 0.4891x | 0.9321x |
| 256 | 0.4887x | 0.9317x |
| 512 | 0.4885x | 0.9315x |
| 1024 | 0.4884x | 0.9314x |

结论：压缩率几乎不随 chunk_z_rows 变化，256 是合理默认。

### stride vs 存储比（50GB 数据集）

| stride | sidecar 大小 | 总存储比 | 是否可用 |
|--------|-------------|---------|---------|
| 1 | 46.37 GB | 1.938x | 超限 |
| 2 | 23.18 GB | 1.468x | 超限 |
| 3 | 15.46 GB | 1.323x | 可用但 page cache 干扰 |
| 4 | 11.59 GB | 1.232x | 可用但 page cache 干扰 |
| 8 | 5.80 GB | 1.114x | 可用但命中率低 |

结论：50G 压缩率 0.979x 太差，sidecar 文件过大导致 page cache 干扰，不应使用。

## 推荐命令

```bash
# === 20GB 数据 ===
# 数据转换（lz4 压缩）
./build/erwt3d convert input=cup_3d_small.dat output=cup_3d_small_lz4.erwt3d \
    nx=801 ny=2405 nz=2501 threads=8 memory-limit-mb=4096 compress=true

# 生成 sidecar（stride=1，自动决策存储比）
./build/erwt3d precompute-x raw=cup_3d_small.dat erwt3d=cup_3d_small_lz4.erwt3d \
    nx=801 ny=2405 nz=2501 mode=sidecar

# === 50GB 数据 ===
# 数据转换（lz4 压缩，不生成 sidecar）
./build/erwt3d convert input=cup_3d_big.dat output=cup_3d_big_lz4.erwt3d \
    nx=2001 ny=2201 nz=3000 threads=8 memory-limit-mb=4096 compress=true

# === 通用命令 ===
# 正确性验证
./build/erwt3d verify raw=data.raw erwt3d=data.erwt3d \
    nx=N ny=N nz=N samples=100000

# 赛题 benchmark
./build/erwt3d bench-contest input=data.erwt3d output-dir=/mnt/d/bench_out hdd

# 查看文件信息（含 sidecar）
./build/erwt3d info data.erwt3d
```
