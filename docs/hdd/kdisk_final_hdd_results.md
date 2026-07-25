# HDD Final Validation (PR #61 merged, commit 06348fe)

## 环境
- 代码：main @ `06348fe`
- CTest：39/39 ✅
- K 盘：`/mnt/k`（hgfs HDD）— 转换输出 + 切片
- G 盘：`/home`（NVMe xfs 系统盘）— 仅结果对比

## 旧基线（G 盘，多文件格式，PR #61 之前）

| 数据集 | 格式 | Cold r1 |
|--------|------|:------:|
| 20GB | LZ4 + YZ sidecar | 16.96s |
| 20GB | LZ4 basic（无 sidecar） | 32.49s |
| 50GB | RZFP axis leaf | 31.08s |
| 50GB | RZFP legacy（无 axis） | 66.18s |

## 转换（K 盘）

| 指标 | 20GB | 50GB |
|------|------|------|
| HDD RAM staging | 18GB@160MiB/s | 21GB@345MiB/s |
| LZ4 main encode | 168s | — |
| RZFP encode | — | 2235s |
| Y/Z section | 57s + 41s | — |
| Axis repack | — | 1017s |
| Package assembly | 91s | 1268s |
| **转换总时间** | **484s（8分钟）** | **4582s（76分钟）** |
| 存储比 | 0.918x | 1.295x |
| Embedded axes | YZ | XYZ |
| Violations | — | 0 |

## 切片性能（Cold r1）

| 数据集 | K 盘 | G 盘 | vs 旧基线 |
|--------|:----:|:----:|:--------:|
| **20GB** | **12.53s** | 19.56s | **-26.1%** |
| **50GB** | **17.49s** | 24.64s | **-43.7%** |

- IO profile: auto → hdd
- 20GB: Fast-path YZ (embedded), Read tuning window=128MiB
- 50GB: Fast-path XYZ axis-leaf (embedded)

## 验收
- 330/330 SHA256 MATCH ✅
- Storage ≤ 1.50x ✅
- Violations = 0 ✅
