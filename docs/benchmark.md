# 性能测试

## 比赛口径

赛题2 的综合时间公式保持不变：

```text
T_composite =
(
  T_x_random +
  T_y_random +
  T_z_random +
  T_x_continuous +
  T_y_continuous +
  T_z_continuous
) / 6
```

六组测试分别是：

- X random：X 方向随机切片 100 次
- Y random：Y 方向随机切片 100 次
- Z random：Z 方向随机切片 100 次
- X continuous：X 方向连续大切片 10 次
- Y continuous：Y 方向连续大切片 10 次
- Z continuous：Z 方向连续大切片 10 次

预处理时间不计入性能分，但预处理后生成的全部必要文件计入存储分。严格口径下，读取结束标志是完成标准 raw 文件写出。

## 推荐命令

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/erwt3d_verify \
  --raw data.dat \
  --erwt3d data.erwt3d \
  --nx NX --ny NY --nz NZ \
  --samples 100000 \
  --rel-tol 1e-3

./build/erwt3d_bench_contest \
  --input data.erwt3d \
  --output-dir bench_out \
  --random-count 100 \
  --continuous-count 10 \
  --continuous-start random \
  --timing-mode strict \
  --storage-path data.erwt3d \
  --hdd
```

也可以直接用脚本：

```bash
./scripts/benchmark_contest_strict.sh data.erwt3d bench_out
./scripts/verify_contest.sh data.dat data.erwt3d NX NY NZ
```

## `erwt3d_bench_contest` 新口径

`--timing-mode strict` 是推荐正式口径。计时覆盖：

```text
open/create output files
read ERWT3D slices
decode/decompress/reorder
write standard raw files
close output files
```

`--timing-mode fast` 保留了预创建输出文件的做法，只适合调试读取核心，不应作为正式比赛成绩。

`--fsync-output` 默认关闭。只有显式传入时，才会在 close 前调用 `fsync(fd)`。

## 连续切片起点

`--continuous-start` 支持三种模式：

- `random`：默认，按 seed 在合法范围内随机起点
- `middle`：用于复现实验
- `zero`：用于边界测试

当前 benchmark 会把 `continuous_start_mode` 和每个轴的实际起点写入 `contest_score.csv`。

## 存储统计

`--storage-path` 支持：

- 普通文件：统计该文件大小
- 目录：递归统计目录下所有普通文件大小

如果不传 `--storage-path`，默认只统计 `--input` 文件大小以保持兼容。正式测试建议显式传入完整预处理输出目录或必要文件路径。

存储分规则保持：

```text
storage_ratio <= 1.5 -> 20 分
超过 1.5 后，每超过 0.1 扣 1 分
```

## repeats 解释

`--repeats` 默认仍为 `1`。当 `repeats > 1` 时：

- `group_time_ms` 仍保留 min，兼容旧输出
- 同时输出 `group_time_min_ms`、`group_time_mean_ms`、`group_time_median_ms`、`group_time_max_ms`
- 终端会提示 min 结果偏乐观

正式报告建议使用 `repeats=1`，或者至少同时给出 mean/median。

## 输出目录说明

输出目录位置会显著影响结果：

- 输出到 SSD 或 tmpfs：更偏向读取算法性能
- 输出到同一块 HDD：更接近总 I/O 压力

正式报告需要写清输出目录所在磁盘，否则 benchmark 结果不可比。

## 正确性判据

`erwt3d_verify` 默认按相对误差判失败：

```text
abs(raw) <= zero_abs_tol 且未启用 strict-relative:
  用绝对误差 zero_abs_tol 保护接近 0 的点
否则:
  用相对误差 rel_tol 判定
```

默认参数：

- `--rel-tol 1e-3`
- `--zero-abs-tol 1e-6`

如果要完全按“所有点都看相对误差”模拟，可加 `--strict-relative`。

## 主维度单列读取

项目支持主维度单列读取：

- `erwt3d_line`：读取单条线并写出标准 raw
- `erwt3d_bench_line`：随机主维度单列 benchmark，输出 `line_benchmark.csv`

这部分能力不计入当前 60 分性能公式，但属于题目要求的功能支持范围。
