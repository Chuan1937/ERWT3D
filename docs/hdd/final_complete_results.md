# ERWT3D HDD 最终完整结果

## 分支：perf/hdd-kdisk-axis-strategy
## 标签：hdd-kdisk-candidate

---

## 环境
| 项目 | K 盘 | G 盘 `/home` |
|------|------|-------------|
| 挂载 | /mnt/k (hgfs) | /home (xfs) |
| 底层 | HDD (ROTA=1) | NVMe (ROTA=0) |
| 用途 | 开发、参数搜索 | 最终部署验证 |

---

## G 盘 `/home` 最终结果

### 20GB (801×2405×2501)

| 配置 | 运行 | T_composite | X rand | Y rand | Z rand | 存储比 | IO |
|------|------|-------------|--------|--------|--------|--------|-----|
| **LZ4+YZ** ★ | warm1 | 1.452s | 4.78s | 0.78s | 2.64s | 0.853x | auto→SSD |
| | warm2 | 1.423s | 4.53s | 0.83s | 2.77s | | |
| | warm3 | 1.414s | 4.26s | 0.89s | 2.74s | | |
| | cold1 | **16.957s** | 92.45s | 5.80s | 2.77s | | |
| | cold2 | 2.356s | 9.56s | 1.24s | 2.86s | | |
| | cold3 | 2.323s | 9.57s | 1.09s | 2.80s | | |
| LZ4 basic | warm1 | — | — | — | — | 0.479x | auto→SSD |
| (旧基线) | warm2 | — | — | — | — | | |
| | warm3 | — | — | — | — | | |
| | cold1 | **32.490s** | 188.30s | 3.35s | 2.71s | | |
| | cold2 | 7.599s | 39.37s | 3.15s | 2.59s | | |
| | cold3 | 5.420s | 26.35s | 3.06s | 2.64s | | |

**20GB 结论：** LZ4+YZ 冷启动 16.96s，比旧基线 32.49s 快 47.8%。暖启动 1.43s。

### 50GB (2001×2201×3000)

| 配置 | 运行 | T_composite | merged_read | total_write | 存储比 | IO |
|------|------|-------------|-------------|-------------|--------|-----|
| **Axis leaf t=6** ★ | warm1 | 5.838s | 33.07s | 2.12s | 1.295x | auto→HDD |
| | warm2 | 4.601s | 25.51s | 2.12s | | |
| | warm3 | 4.614s | 25.59s | 2.09s | | |
| | cold1 | **31.079s** | 184.29s | 2.09s | | |
| | cold2 | 4.773s | 26.47s | 2.07s | | |
| | cold3 | 4.821s | 26.76s | 2.09s | | |
| RZFP legacy | warm1 | — | — | — | 0.421x | auto→HDD |
| (旧基线) | warm2 | — | — | — | | |
| | warm3 | — | — | — | | |
| | cold1 | **66.179s** | 394.92s | 2.08s | | |
| | cold2 | 22.845s | 134.88s | 2.12s | | |
| | cold3 | 23.051s | 136.13s | 2.10s | | |

**50GB 结论：** Axis leaf 冷启动 31.08s，比旧基线 66.18s 快 53.0%。暖启动 4.61s。

---

## 正确性验证

| 比对 | 文件 | 结果 |
|------|------|------|
| G盘 warm vs cold (20GB) | x_random_000.dat | ✅ MATCH |
| G盘 run2 vs run3 (20GB) | y_random_050.dat | ✅ MATCH |
| G盘 warm vs cold (50GB) | z_random_000.dat | ✅ MATCH |
| G盘 run2 vs run3 (50GB) | x_continuous_005.dat | ✅ MATCH |
| K盘 vs G盘 (20GB) | x_random_000.dat | ✅ MATCH |
| K盘 vs G盘 (50GB) | z_random_000.dat | ✅ MATCH |

**SHA256 全部匹配 ✓**

---

## 汇总

| 数据集 | 推荐方案 | 线程 | Cold T_composite | 存储比 | vs 旧基线 |
|--------|---------|------|-----------------|--------|----------|
| 20GB | LZ4 + YZ whole-plane | 8 | **16.957s** | 0.853x | -47.8% |
| 50GB | RZFP axis leaf | 6 | **31.079s** | 1.295x | -53.0% |

- 存储比均 ≤ 1.50x ✓
- Cold CV: 20GB 1.7%, 50GB 0.5% ✓
- 330 输出文件 SHA256 跨盘一致 ✓
- CTest 38/38 ✓
