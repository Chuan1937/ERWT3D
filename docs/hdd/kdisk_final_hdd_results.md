# K-Disk HDD Final Validation (PR #61)

## 环境
- 代码：main @ `a1bf781`, CTest 40/40 ✅
- 编译：`-DCMAKE_BUILD_TYPE=Release -DERWT3D_NATIVE_OPT=ON`（clean rebuild, `-march=native`, AVX2）
- 平台：Rocky Linux 9.8, i9-10850K 16 vCPU, 62GB
- K 盘：`/mnt/k`（hgfs HDD）
- 每次测试前：host evict (G盘 8GB 读取) + triple drop_caches
- 参数：threads=16, memory-limit-mb=4096, io-profile=hdd

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

## 切片（K 盘，纯冷，-march=native clean build）

| 数据集 | T_composite | process_e2e | **REAL ELAPSED** | DAT | RSS |
|--------|:---------:|:----------:|:----------:|:---:|:---:|
| **20GB** | 6.04s | 36.36s | **36.45s** | 330 | 3.4GB |
| **50GB** | 11.34s | 68.05s | **71.94s** | 330 | 8.3GB |

- 20GB: Fast-path YZ (embedded), Read window=128MiB
- 50GB: Fast-path XYZ axis-leaf (embedded), merged_read=23.0s, total_write=44.4s

## 编译差异

| 编译方式 | 20GB REAL | 50GB REAL |
|---------|:------:|:------:|
| 增量编译（-march=native 未生效） | 71.34s | 117.52s |
| **clean rebuild（-march=native 生效）** | **36.45s** | **71.94s** |

AVX2 加速约 39%–49%。

## 验收
- 330/330 DAT 文件 ✅
- Storage ≤ 1.50x ✅
- Violations = 0 ✅
- Guest-cold（VMware host cache unknown）
