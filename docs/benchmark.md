# 性能测试

## 评分公式

```
T_composite = (T_xr + T_yr + T_zr + T_xc + T_yc + T_zc) / 6
得分 = (基准时间 / T_composite) × 60
```

## 测试环境

D盘机械硬盘，WSL 9p 挂载。配置：`--hdd`（128MB 读窗口，1MB gap，单线程）

## 最新结果（X-plane 优化）

### 20GB（801×2405×2501）

| 测试项 | 无 X-plane | 有 X-plane | 提速 |
|--------|-----------|------------|------|
| X random (100片) | 74s | 25s | 2.9x |
| Y random (100片) | 63s | 62s | — |
| Z random (100片) | 58s | 58s | — |
| X continuous (10片) | 6s | 1.6s | 3.8x |
| Y continuous (10片) | 1.8s | 1.9s | — |
| Z continuous (10片) | 1.7s | 2.0s | — |
| **T_composite** | **31.95s** | **25.18s** | **21.2%** |

存储比：2.075x → 15/20 分

### 50GB（2001×2201×3000）

| 测试项 | 无 X-plane | 有 X-plane | 提速 |
|--------|-----------|------------|------|
| X random (100片) | 167s | 29s | 5.7x |
| Y random (100片) | 160s | 185s | -15% |
| Z random (100片) | 151s | 157s | -4% |
| X continuous (10片) | 6s | 1.7s | 3.5x |
| Y continuous (10片) | 5s | 15s | -3x |
| Z continuous (10片) | 4s | 4s | — |
| **T_composite** | **82.37s** | **65.30s** | **20.7%** |

存储比：~1.94x → 16/20 分

## 内存限制对比（无 X-plane，基线）

### 20GB

| MemLimit | X random | Y random | Z random | X cont | Y cont | Z cont | T_composite |
|----------|----------|----------|----------|--------|--------|--------|-------------|
| 2 GB | 142.37s | 63.27s | 58.09s | 6.68s | 2.01s | 1.95s | 45.73s |
| 4 GB | 73.78s | 63.42s | 57.97s | 7.11s | 2.18s | 2.06s | 34.42s |
| 8 GB | 75.00s | 63.39s | 57.57s | 6.59s | 2.04s | 1.91s | 34.41s |
| 16 GB | 74.36s | 63.16s | 59.28s | 6.91s | 2.11s | 2.15s | 34.66s |
| 32 GB | 75.61s | 62.29s | 58.39s | 6.71s | 1.99s | 1.97s | 34.50s |
| 64 GB | 73.74s | 64.40s | 57.83s | 6.78s | 2.10s | 2.08s | 34.49s |

### 50GB

| MemLimit | X random | Y random | Z random | X cont | Y cont | Z cont | T_composite |
|----------|----------|----------|----------|--------|--------|--------|-------------|
| 2 GB | 312.93s | 240.48s | 158.79s | 7.53s | 6.28s | 4.44s | 121.74s |
| 4 GB | 177.50s | 171.85s | 158.58s | 7.53s | 5.94s | 4.61s | 87.67s |
| 8 GB | 177.45s | 170.11s | 169.23s | 7.21s | 5.71s | 4.30s | 89.00s |
| 16 GB | 178.15s | 171.63s | 158.22s | 11.24s | 5.93s | 4.30s | 88.24s |
| 32 GB | 178.71s | 170.29s | 159.14s | 7.43s | 5.67s | 4.32s | 87.59s |
| 64 GB | 182.75s | 176.81s | 158.31s | 7.30s | 5.73s | 4.32s | 89.21s |

## 推荐命令

```bash
# 转换（带 X-plane）
./build/erwt3d_convert --input data.raw --output data.erwt3d \
    --nx 801 --ny 2405 --nz 2501 --threads 8 --memory-limit-mb 4096
./build/erwt3d_precompute_x --raw data.raw --erwt3d data.erwt3d \
    --nx 801 --ny 2405 --nz 2501

# 测试
./build/erwt3d_bench_contest --input data_xp.erwt3d --output-dir out --hdd
```
