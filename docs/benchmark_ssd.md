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

| 配置 | X random | Y random | Z random | T_total |
|------|----------|----------|----------|---------|
| Serial t1 | 0.3730s | 0.1490s | 0.1990s | 0.2490s |
| **ParallelRead t8** | **0.1550s** | **0.0450s** | **0.0400s** | **0.0730s** |

### 50G 数据集 (2001×2201×3000)

| 配置 | X random | Y random | Z random | T_total |
|------|----------|----------|----------|---------|
| Serial t1 | 2.1590s | 0.9410s | 0.5320s | 0.7700s |
| **ParallelRead t8** | **0.4990s** | **0.5630s** | **0.3770s** | **0.3000s** |

## 推荐命令

```bash
erwt3d bench_ssd input=data.erwt3d output=/tmp/out
```
