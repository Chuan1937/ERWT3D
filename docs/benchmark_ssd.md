# SSD 性能测试

## 测试环境

| 项目 | 配置 |
|------|------|
| CPU | Intel i7-13700F, 24 线程 |
| RAM | 64 GB |
| SSD | 系统盘 C/D |

## 评分公式

```
T_composite = (T_xr + T_yr + T_zr + T_xc + T_yc + T_zc) / 6
得分 = (基准时间 / T_composite) × 60
```

## 测试结果

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

## 推荐命令

```bash
erwt3d bench_ssd input=data.erwt3d output=/tmp/out
```
