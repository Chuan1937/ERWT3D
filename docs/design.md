# 存储结构与算法

## 文件格式

```
┌──────────────────┐
│ Header 256B      │  magic, version, nx/ny/nz, block sizes, flags
├──────────────────┤
│ Superblock Data  │  按 Z-Y-X 顺序排列（Z 为最外层）
│                  │  每个 Superblock 内 Leaf blocks 按 Morton 序排列
├──────────────────┤
│ (可选) X-Panel   │  预存 YZ 平面数据，加速 X 切片
├──────────────────┤
│ (可选) X-Plane   │  连续存储的完整 X 切片平面
└──────────────────┘
```

## 分块设计

两级层次结构：

| 层级 | 大小 | 说明 |
|------|------|------|
| Superblock | 64×64×64 float32 = 1 MiB | I/O 单元，物理存储粒度 |
| Leaf block | 4×4×4 float32 = 256 B | 最小逻辑单元，Morton 编址 |

Superblock 网格维度：

```
gridX = ceil(nx / 64)
gridY = ceil(ny / 64)
gridZ = ceil(nz / 64)
```

## Morton Z-order

Superblock 内 leaf blocks 使用三维 Z-order curve 排列：

```
leaf_id = morton3D(lx, ly, lz)
file_offset = superblock_offset + leaf_id × 256B
```

Morton 序保证局部空间邻近性——三维坐标相近的点在文件中也相邻。

## 地址计算

给定全局坐标 (x, y, z)：

```cpp
// 1. 定位 superblock
sx = x / 64;  sy = y / 64;  sz = z / 64
sb_idx = (sz * gridY + sy) * gridX + sx
sb_offset = data_offset + sb_idx × 1MiB

// 2. 定位 leaf block
lx = (x % 64) / 4;  ly = (y % 64) / 4;  lz = (z % 64) / 4
leaf_offset = sb_offset + morton3D(lx, ly, lz) × 256B

// 3. leaf 内偏移
ix = x % 4;  iy = y % 4;  iz = z % 4
value_offset = leaf_offset + (iz * 16 + iy * 4 + ix) × 4B
```

纯公式计算，O(1) 定位，无需索引表。

## Superblock 存储顺序

文件中 superblock 按 Z-Y-X 排列（Z 为最外层）：

```
sb_idx = (sz * gridY + sy) * gridX + sx
```

这意味着：
- **Z 切片**（固定 sz）：触及 gridX × gridY 个连续 superblock → 最快
- **Y 切片**（固定 sy）：触及 gridX × gridZ 个 superblock，每行 gridX 个连续 → 中等
- **X 切片**（固定 sx）：触及 gridY × gridZ 个 superblock，间隔 gridX 个 → 最慢

## 切片读取流程

```
切片请求 → Plan 构建 → 文件偏移排序 → 读窗口合并 → 顺序 pread → 解包 → 输出
```

### Plan 构建

计算切片涉及的所有 (superblock, leaf) 对：

```cpp
// X 切片示例
for (szi in 0..gridZ-1):
    for (syi in 0..gridY-1):
        sb_idx = (szi * gridY + syi) * gridX + superX
        // 计算需要的 leaf blocks...
```

### 文件偏移排序

将所有 task 按 file_offset 升序排列，最小化磁头移动距离。

### 读窗口合并

将相邻的 superblock 读取合并为大窗口：

```
窗口参数（--hdd 模式）：
  read_window_bytes = 128 MiB    单次 pread 最大大小
  max_gap_bytes     = 1 MiB     允许合并的最大间隔
```

合并逻辑：若两个相邻 task 的文件间隔 ≤ max_gap 且合并后 ≤ read_window，则合并为一次 pread。

### 顺序读取

单线程顺序 pread，配合内核预取：

```cpp
posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);  // 提示顺序访问
readahead(fd, future_offset, future_size);          // 预取后续窗口
```

### 解包

从 superblock 缓冲区提取切片数据，写入输出缓冲区。

## 各轴访问特征（20GB: 801×2405×2501）

| 轴 | superblock 触及数 | 间隔 | pread 调用/切片 |
|----|-------------------|------|-----------------|
| Z  | 13 × 38 = 494 | 连续 | ~40（窗口合并后） |
| Y  | 13 × 40 = 520 | 13MB 行内连续 | ~520 |
| X  | 38 × 40 = 1520 | 13MB / 494MB | ~1520 |

## 存储比例

| 数据集 | 原始大小 | 文件大小 | 比例 |
|--------|----------|----------|------|
| 20GB (801×2405×2501) | 18.0 GB | 19.3 GB | 1.075x |
| 50GB (2001×2201×3000) | 49.2 GB | 51.3 GB | 1.044x |

## 可选扩展：X-Panel

convert 时加 `--panel-axis x --panel-stride N`，在每个 superblock 内预存每 N 个本地 X 值的 YZ 平面：

```
每个 superblock 增加: (64/N) × 64 × 64 × 4B
stride=4: 增加 16 × 16KB = 256KB/superblock → 存储增加 ~27%
```

仅覆盖 stride 整除的 X 值，其余仍走 superblock 路径。

## 可选扩展：X-Plane

使用 `erwt3d_precompute_x` 工具，在文件末尾追加连续 X 切片平面数据：

```
每个 X 平面: ny × nz × 4B（20GB 数据集约 23MB/平面）
stride=1: 存储翻倍（2x）
stride=4: 存储增加 ~27%（1.27x）
```

读取 X 切片时，若该 X 值有对应平面，只需 1 次 pread 即可读取完整切片。
