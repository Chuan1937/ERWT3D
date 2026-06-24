# 脚本

数据在 D 盘 HDD（`/mnt/d/CUP/`）。

## 性能测试

| 脚本 | 功能 |
|------|------|
| `bench_single.sh` | 单切片延迟测试 |
| `benchmark.sh` | 赛题标准 benchmark（100 random + 10 continuous） |
| `bench_mem_sweep.sh` | 内存限制扫描（2-64GB × 20GB/50GB） |

## 数据验证

| 脚本 | 功能 |
|------|------|
| `convert.sh` | RAW → ERWT3D 转换 + 验证 |
| `verify.sh` | ERWT3D ↔ RAW 逐点比对 |

## 用法

```bash
# 赛题 benchmark
bash scripts/benchmark.sh

# 内存扫描（约 1-2 小时）
bash scripts/bench_mem_sweep.sh

# 验证数据
bash scripts/verify.sh /mnt/d/CUP/cup_3d_small.dat /mnt/d/CUP/cup_3d_small.erwt3d

# 转换 + 验证
bash scripts/convert.sh /mnt/d/CUP/cup_3d_small.dat /mnt/d/CUP/test/out.erwt3d
```
