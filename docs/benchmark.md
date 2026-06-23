# 性能测试

## 评分公式

```
T_composite = (T_xr + T_yr + T_zr + T_xc + T_yc + T_zc) / 6
得分 = (基准时间 / T_composite) × 60
```

## 测试结果

- [HDD 测试结果](benchmark_hdd.md)

## 推荐命令

```bash
./build/erwt3d_bench_contest --input data.erwt3d --output-dir out --hdd
```
