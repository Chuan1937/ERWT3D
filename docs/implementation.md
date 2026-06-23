# 算法实现

## Writer（写入）

```
原始数据 → 逐 superblock 处理 → Morton 序 leaf blocks → 写入文件
```

关键步骤：
1. 从原始数据提取 64³ superblock
2. 边界不足部分补零
3. 按 Morton 序写入 leaf blocks

## Reader（读取）

```
切片请求 → Plan → 排序 → 合并读取 → 解包 → 输出
```

关键步骤：
1. **Plan**: 计算涉及的 superblocks 和 leaf blocks
2. **排序**: 按文件偏移排序
3. **合并**: 将相邻读取合并为大块（128MB 窗口 + 1MB gap）
4. **读取**: 单线程顺序 pread
5. **解包**: 从 superblock 提取切片数据

## 内存控制

```bash
--memory-limit-mb 4096
```

- I/O 缓冲区分批处理
- 每线程独立缓冲区，无锁竞争
