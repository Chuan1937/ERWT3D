# 运维脚本

性能测试与基准扫描，数据在 D 盘 HDD（`/mnt/d/CUP/`）。

| 脚本 | 功能 |
|------|------|
| `bench_single.sh` | 单切片延迟测试 |
| `benchmark.sh` | 赛题标准 benchmark（100+10） |
| `bench_mem_sweep.sh` | 内存限制扫描（2-64GB × 20GB/50GB） |

```bash
# 赛题 benchmark
bash scripts/benchmark.sh /mnt/d/CUP/cup_3d_small.erwt3d

# 内存扫描（约 1-2 小时）
bash scripts/bench_mem_sweep.sh
```
