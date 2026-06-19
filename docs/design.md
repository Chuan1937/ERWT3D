# 存储结构设计

## 文件格式

```
┌──────────────┐
│ Header 256B  │  magic, version, nx/ny/nz, block sizes
├──────────────┤
│ Data Area    │  Superblocks 按 Z-Y-X 顺序排列
│              │  每个 Superblock 内 Leaf blocks 按 Morton 序排列
└──────────────┘
```

## 分块设计

| 层级 | 大小 | 说明 |
|------|------|------|
| Superblock | 64×64×64 float32 = 1 MiB | I/O 单元 |
| Leaf block | 4×4×4 float32 = 256 B | 最小访问单元 |

## Morton 序

Superblock 内 leaf blocks 使用 Z-order curve 排列：

```
leaf_id = morton3D(lx, ly, lz)
file_offset = superblock_offset + leaf_id × 256
```

**优势**: X/Y/Z 三轴访问均衡，无需冗余副本。

## 地址计算

```cpp
// Superblock 偏移
sb_idx = (sz * gridY + sy) * gridX + sx
sb_offset = data_offset + sb_idx × 1MiB

// Leaf 偏移
leaf_offset = sb_offset + morton3D(lx, ly, lz) × 256B
```

无需索引表，纯公式计算。

## 存储比例

| 数据集 | 原始大小 | 文件大小 | 比例 |
|--------|----------|----------|------|
| 20G (801×2405×2501) | 18.0 GB | 19.3 GB | 1.075x |
| 50G (2001×2201×3000) | 49.2 GB | 51.3 GB | 1.044x |
