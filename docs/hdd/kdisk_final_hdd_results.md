# HDD Final Validation (PR #61 merged, commit 06348fe)

## 环境
- 代码：main @ `06348fe`, CTest 39/39
- 转换 + 切片：K 盘 `/mnt/k`
- 纯冷：host evict + triple drop_caches

## 转换（K 盘）

| 指标 | 20GB | 50GB |
|------|------|------|
| HDD RAM staging | 18GB@160MiB/s | 21GB@345MiB/s |
| 主编码 | LZ4 168s | RZFP 2235s |
| Axis 生成 | Y57s+Z41s | repack 1017s |
| 程序集 | 91s | 1268s |
| **总转换** | **484s（8分）** | **4582s（76分）** |
| 存储比 | 0.918x | 1.295x |
| Violations | — | 0 |

## 切片（K 盘，纯冷，io-profile auto）

| 数据集 | T_composite | **process_e2e** | Fast-path |
|--------|:---------:|:----------:|-----------|
| **20GB** | **12.96s** | **77.81s** | YZ (embedded) |
| **50GB** | **20.61s** | **123.67s** | XYZ axis-leaf (embedded) |

## 验收
- 330/330 SHA256 MATCH ✅
- Storage ≤ 1.50x ✅
- Violations = 0 ✅
