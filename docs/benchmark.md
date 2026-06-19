# 性能测试

## 测试环境

| 项目 | 配置 |
|------|------|
| CPU | Intel i7-13700F, 24 线程 |
| RAM | 64 GB |
| SSD | 系统盘 C/D |
| HDD | 数据盘 F/G |

## 评分公式

```
T_composite = (T_xr + T_yr + T_zr + T_xc + T_yc + T_zc) / 6
得分 = (基准时间 / T_composite) × 60
```

---

## SSD 测试结果

### 20G 数据集 (801×2405×2501)

| 配置 | T_x_rand | T_y_rand | T_z_rand | T_total |
|------|----------|----------|----------|---------|
| Serial t1 | 373ms | 149ms | 199ms | 249ms |
| **ParallelRead t8** | **155ms** | **45ms** | **40ms** | **73ms** |

### 50G 数据集 (2001×2201×3000)

| 配置 | T_x_rand | T_y_rand | T_z_rand | T_total |
|------|----------|----------|----------|---------|
| Serial t1 | 2159ms | 941ms | 532ms | 770ms |
| **ParallelRead t8** | **499ms** | **563ms** | **377ms** | **300ms** |

---

## HDD 测试结果

### 20G 数据集 (801×2405×2501)

**关键发现**: HDD 必须单线程顺序读取，多线程并发会导致磁头跳动。

#### 模式对比

| 模式 | X random | Y random | Z random | T_total |
|------|----------|----------|----------|---------|
| 逐切片读取 | 4673ms | 2443ms | 2637ms | ~3200ms |
| **Batch planner** | **927ms** | **1404ms** | **1263ms** | **877ms** |

#### 推荐配置

```bash
./build/erwt3d_bench \
  --input data.erwt3d \
  --output-dir hdd_bench \
  --random-count 100 \
  --continuous-count 10 \
  --threads 1 \
  --memory-limit-mb 4096 \
  --io-backend sb \
  --sb-task-order file-offset \
  --sb-read-mode hdd-read-window \
  --hdd-read-window-bytes 33554432 \
  --hdd-max-gap-bytes 1048576 \
  --hdd-batch-planner on \
  --hdd-batch-window-bytes 33554432 \
  --hdd-batch-max-gap-bytes 1048576
```

**注意**: HDD 模式使用 `--threads 1`，多线程会导致磁头跳动反而更慢。

---

## SSD vs HDD 对比

| 指标 | SSD | HDD | 差距 |
|------|-----|-----|------|
| T_total (20G) | 73ms | 877ms | 12x |
| T_x_random | 155ms | 927ms | 6x |
| T_y_random | 45ms | 1404ms | 31x |
| T_z_random | 40ms | 1263ms | 32x |

**结论**: HDD 比 SSD 慢 12-32 倍，主要瓶颈是随机寻道时间。

---

## 推荐命令

### SSD 环境

```bash
./build/erwt3d_bench_contest -i data.erwt3d -o out -t 8 -m 8192 \
  --io-backend sb --sb-parallel-mode parallel-read --sb-task-order file-offset
```

### HDD 环境

```bash
./build/erwt3d_bench_contest -i data.erwt3d -o out --hdd
```

`--hdd` 自动设置: 单线程、4GB 内存、顺序读取、批量合并。

### HDD 环境

```bash
./build/erwt3d_bench_contest \
  --input data.erwt3d \
  --output-dir out \
  --threads 8 \
  --memory-limit-mb 8192 \
  --io-backend sb \
  --sb-parallel-mode parallel-read \
  --sb-task-order file-offset \
  --sb-read-mode hdd-read-window \
  --hdd-read-window-bytes 33554432 \
  --hdd-max-gap-bytes 1048576
```
