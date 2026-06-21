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

## 测试结果

- [SSD 测试结果](benchmark_ssd.md)
- [HDD 测试结果](benchmark_hdd.md)

## SSD vs HDD 对比

| 指标 | SSD | HDD | 差距 |
|------|-----|-----|------|
| T_total (20G) | 73ms | 877ms | 12x |
| T_x_random | 155ms | 927ms | 6x |
| T_y_random | 45ms | 1404ms | 31x |
| T_z_random | 40ms | 1263ms | 32x |

**结论**: HDD 比 SSD 慢 12-32 倍，主要瓶颈是随机寻道时间。

## 推荐命令

### SSD 环境

```bash
erwt3d bench_ssd input=data.erwt3d output=/tmp/out
```

### HDD 环境

```bash
erwt3d bench_hdd input=data.erwt3d output=/tmp/out
```
