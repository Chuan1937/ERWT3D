# K-Disk HDD Final Validation (PR #61 merged)

## 环境
- 代码：main @ `06348fe` (Merge PR #61)
- CTest：39/39 ✅
- 转换二进制：系统 NVMe
- 原始 DAT：G 盘 `/mnt/g/CUP/`
- 转换输出 + 切片：K 盘 `/mnt/k/K/erwt3d_hdd_validation/`

## 转换

| 指标 | 20GB | 50GB |
|------|------|------|
| HDD RAM staging | ✅ 18378MiB@160MiB/s | ✅ 21214MiB@345MiB/s |
| 主编码 | LZ4 encode 168s | RZFP encode 2235s |
| Axis 生成 | Y 57s + Z 41s | repack 1017s |
| 程序集 | 91s (kernel-copy) | 1268s (kernel-copy) |
| **转换总时间** | **484s（8分钟）** | **4582s（76分钟）** |
| 总（plan+convert） | 703s（12分钟） | 4931s（82分钟） |
| 存储比 | 0.918x | 1.295x |
| 嵌入式轴 | YZ | XYZ |
| Violations | — | 0 |
| Max relative error | — | 0.0009999999 |

## 切片性能（Guest-cold，drop_caches，K 盘）

| 数据集 | Cold r1 | Cold r2 | Cold r3 | Warm |
|--------|---------|---------|---------|------|
| **20GB** | **12.53s** | 6.30s | 6.33s | 6.14s |
| **50GB** | **17.49s** | 12.15s | 12.21s | 12.18s |

## 对比旧基线

| 数据集 | 旧 Cold r1 | 新 Cold r1 | 改善 |
|--------|-----------|-----------|------|
| 20GB LZ4 YZ | 16.96s | 12.53s | **-26.1%** |
| 50GB RZFP axis | 31.08s | 17.49s | **-43.7%** |

## 验收
- 330/330 SHA256 MATCH ✅
- Fast-path: 20GB YZ (embedded), 50GB XYZ (embedded) ✅
- Storage ≤ 1.50x ✅
- Violations = 0 ✅
