# K-Disk HDD Final Validation (PR #61, commit a1bf781)

## 环境
- 代码：main @ `a1bf781`, CTest 39/39 ✅
- 转换 + 切片全在 K 盘 `/mnt/k`
- 参数：threads=16, memory-limit-mb=4096, io-profile=hdd
- 每次测试前：host evict (8GB G盘读取) + triple drop_caches
- Guest-cold, VMware host cache unknown

## 转换

| 指标 | 20GB | 50GB |
|------|------|------|
| HDD RAM staging | 18GB@160MiB/s | 21GB@345MiB/s |
| 主编码 | LZ4 168s | RZFP 2235s |
| Axis 生成 | Y57s+Z41s | repack 1017s |
| 程序集 | 91s | 1268s |
| **转换总时间** | **484s（8分）** | **4582s（76分）** |
| 存储比 | 0.918x | 1.295x |
| Embedded axes | YZ | XYZ |
| Violations | — | 0 |

## 切片性能（K 盘纯冷）

| 数据集 | T_composite | process_e2e | **REAL ELAPSED** | DAT | RSS |
|--------|:---------:|:----------:|:----------:|:---:|:---:|
| **20GB** | 11.85s | 71.17s | **71.34s** | 330 | 3.4GB |
| **50GB** | 18.60s | 111.62s | **117.52s** | 330 | 8.3GB |

- 20GB: Fast-path YZ (embedded), Read window=128MiB
- 50GB: Fast-path XYZ axis-leaf (embedded), merged_read=66.3s, total_write=44.6s

## 验收
- 330/330 DAT 文件 ✅
- Storage ≤ 1.50x ✅
- Violations = 0 ✅
- REAL ELAPSED ≈ process_e2e ✅

## 说明
- REAL ELAPSED 为 `date +%s%N` 实测墙钟时间
- VMware 宿主机缓存不可控，结果标为 Guest-cold
