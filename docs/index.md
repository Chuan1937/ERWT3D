# 索引原理

## 公式索引

ERWT3D 不使用显式索引表，通过公式直接计算偏移：

```cpp
offset = data_offset + ((sz * gridY + sy) * gridX + sx) * sb_bytes
       + morton3D(lx, ly, lz) * leaf_bytes
```

**优势**: 零索引开销，O(1) 定位。

## Morton 编码

将 3D 坐标编码为 1D，交错 x/y/z 的比特位：

```
x: x2 x1 x0
y: y2 y1 y0
z: z2 z1 z0
morton: z2 y2 x2 z1 y1 x1 z0 y0 x0
```

## 切片编译

切片请求 → 编译为 I/O 任务：

1. 确定涉及的 superblocks
2. 确定每个 superblock 内的 leaf blocks
3. 计算文件偏移
4. 合并相邻读取（extent merging）
5. 生成输出拷贝指令

## Extent 合并

相邻读取合并为单次 pread，减少系统调用：

```
合并前: [1000, 256] [1256, 256] [1512, 256]  → 3 次 pread
合并后: [1000, 768]                             → 1 次 pread
```
