# K-Disk HDD Final Validation (PR #61)

## 环境
- 平台：Rocky Linux 9.8, x86-64-v3, i9-10850K 16 vCPU, 62GB
- K 盘：`/mnt/k`（hgfs HDD）
- 参数：threads=16, memory-limit-mb=4096, io-profile=hdd
- 冷缓存：host evict (8GB) + triple drop_caches
- CTest：40/40 ✅

## 转换

| 指标 | 20GB | 50GB |
|------|------|------|
| HDD RAM staging | 18GB@160MiB/s | 21GB@345MiB/s |
| 主编码 | LZ4 168s | RZFP 2235s |
| Axis 生成 | Y57s+Z41s | repack 1017s |
| 程序集 | 91s | 1268s |
| **转换总时间** | **484s（8分）** | **4582s（76分）** |
| 存储比 | 0.918x | 1.295x |
| Violations | — | 0 |

## 切片（三种编译对比）

| 编译方式 | 20GB REAL | 50GB REAL |
|---------|:------:|:------:|
| 通用 `-O3`（增量编译失效） | 71.34s | 117.52s |
| **`-march=x86-64-v3`**（dist包） ★ | **36.45s** | **72.97s** |
| `-march=native` | 36.45s | 71.94s |

x86-64-v3 与 native 性能一致（i9-10850K 最高 AVX2）。

## 最终成绩（x86-64-v3 dist 包）

| 数据集 | T_composite | process_e2e | **REAL ELAPSED** | DAT | RSS |
|--------|:---------:|:----------:|:----------:|:---:|:---:|
| **20GB** | 6.04s | 36.36s | **36.45s** | 330 | 3.4GB |
| **50GB** | 11.66s | 69.98s | **72.97s** | 330 | 8.3GB |

- 20GB: Fast-path YZ (embedded)
- 50GB: Fast-path XYZ axis-leaf (embedded), merged_read=24.1s, total_write=45.2s

## Dist 包

| 文件 | SHA256 |
|------|--------|
| `ERWT3D-1.0.0-linux-rocky9-x86_64-v3.tar.gz` | `3512f90...` |

- 构建：Rocky Linux 9, `-march=x86-64-v3` (AVX2+FMA, Haswell+)
- 含 libzfp, liblz4, libstdc++, libgcc_s
- 用法：`tar xzf *.tar.gz && ./bin/erwt3d_contest --help`

## 验收
- 330/330 DAT ✅
- Storage ≤ 1.50x ✅
- Violations = 0 ✅
- Guest-cold（VMware host cache unknown）
