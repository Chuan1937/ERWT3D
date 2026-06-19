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

| 模式 | T_x_rand | T_y_rand | T_z_rand | T_total |
|------|----------|----------|----------|---------|
| 逐切片 | 1795ms | 609ms | 612ms | 994ms |
| **Batch planner** | **244ms** | **221ms** | **207ms** | **170ms** |

### 线程数对比

| 线程 | T_total |
|------|---------|
| 1 | 395ms |
| 4 | 884ms |
| **8** | **170ms** |
| 16 | 503ms |

---

## SSD vs HDD 对比

| 指标 | SSD | HDD | 差距 |
|------|-----|-----|------|
| T_total (20G) | 49ms | 170ms | 3.5x |
| T_x_random | 62ms | 244ms | 3.9x |
| T_y_random | 35ms | 221ms | 6.3x |

---

## 存储比例

| 数据集 | 比例 |
|--------|------|
| 20G | 1.075x |
| 50G | 1.044x |

---

## 推荐命令

### SSD 环境

```bash
./build/erwt3d_bench_contest \
  --input data.erwt3d \
  --output-dir out \
  --threads 8 \
  --memory-limit-mb 8192 \
  --io-backend sb \
  --sb-parallel-mode parallel-read \
  --sb-task-order file-offset
```

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
