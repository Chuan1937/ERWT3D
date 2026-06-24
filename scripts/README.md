# 脚本

数据在 D 盘 HDD（`/mnt/d/CUP/`）。

所有脚本第一个参数都是数据集选择：`small|big|all`，默认 `small`。

- `small` = `cup_3d_small` (20GB, 801×2405×2501)
- `big`   = `cup_3d_big`   (50GB, 2001×2201×3000)

## 性能测试

| 脚本 | 功能 |
|------|------|
| `bench_single.sh` [dataset] | 单切片延迟（X/Y/Z 各一片） |
| `benchmark.sh` [dataset] | 赛题标准 benchmark（100 random + 10 continuous） |
| `bench_mem_sweep.sh` | 内存限制扫描（2/4/8/16/32/64 GB × 两个数据集，自动跑） |

## 数据验证

| 脚本 | 功能 |
|------|------|
| `convert.sh` [dataset] | RAW → ERWT3D 转换 + 验证 |
| `verify.sh` [dataset] | 现有 ERWT3D ↔ RAW 逐点比对 |

## 用法

```bash
# 20GB（默认）
bash scripts/benchmark.sh
bash scripts/verify.sh

# 50GB
bash scripts/benchmark.sh big
bash scripts/verify.sh big

# 两个数据集都跑
bash scripts/benchmark.sh all
bash scripts/bench_single.sh all
bash scripts/verify.sh all

# 内存扫描（约 1-2 小时，自动跑两个数据集）
bash scripts/bench_mem_sweep.sh
```

## 输出位置

```
/mnt/d/CUP/test_hdd/
├── bench/{small,big}/             # benchmark.sh 输出
├── single/{small,big}/            # bench_single.sh 输出
├── small_test.erwt3d              # convert.sh small 输出
├── big_test.erwt3d                # convert.sh big 输出
└── mem_sweep/                     # bench_mem_sweep.sh 结果 CSV
```