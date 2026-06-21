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

## 推荐命令

### SSD 环境

```bash
erwt3d bench_ssd input=data.erwt3d output=/tmp/out
```

### HDD 环境

```bash
erwt3d bench_hdd input=data.erwt3d output=/tmp/out
```
