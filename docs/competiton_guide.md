# 赛题2：三维空间数据的高效读写

## 目标

ERWT3D 需要在不维护 X/Y/Z 三份完整副本的前提下，支持三方向切片、连续切片和主维度单列读取，并尽量贴近比赛口径完成验证和性能报告。

## 评分口径

功能正确性是否决项，核心要求是：

```text
单点相对误差 < 0.001
```

性能部分使用六组切片时间的平均值：

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

六组测试分别为：

- X random：100 次
- Y random：100 次
- Z random：100 次
- X continuous：10 次
- Y continuous：10 次
- Z continuous：10 次

性能得分依旧是：

```text
性能得分 = (基准时间 / T_composite) × 60
```

其中基准时间是所有参赛者中的最短综合时间，不是固定常数。

## 预处理与存储

- 预处理时间不计入性能分
- 预处理后全部必要文件大小计入存储分
- 存储比不超过原始数据 `1.5x` 可得满分
- 禁止把 X/Y/Z 三份完整副本作为默认方案

`erwt3d_bench_contest` 现在支持 `--storage-path`，可显式统计单文件或整个预处理目录，更接近上述口径。

## 计时边界

推荐正式测试使用：

```bash
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

`--timing-mode strict` 下，计时覆盖：

```text
open/create output files
read ERWT3D slices
decode/decompress/reorder
write standard raw files
close output files
```

`--timing-mode fast` 仍可用于调参，但它会把预创建输出文件放在计时外，因此不是正式比赛口径。

## 正确性验证

推荐命令：

```bash
./build/erwt3d_verify \
  --raw data.dat \
  --erwt3d data.erwt3d \
  --nx NX --ny NY --nz NZ \
  --samples 100000 \
  --rel-tol 1e-3
```

默认策略：

- 主要按相对误差 `--rel-tol` 判定
- 对接近 0 的点，使用 `--zero-abs-tol` 做保护
- 若要完全严格模拟“所有点都按相对误差”，使用 `--strict-relative`

`--raw-a/--raw-b` 模式也采用同样判据。

## 连续切片起点

当前 benchmark 默认使用：

```text
--continuous-start random
```

并且使用 `--seed` 保证可复现。`middle` 和 `zero` 仍保留，用于复现实验和边界测试。

## repeats 解释

比赛正式报告建议使用：

```text
--repeats 1
```

如果做多次重复，当前工具会：

- 仍用每组最小值参与 `T_composite`
- 同时输出每组的 min/mean/median/max
- 在终端提示该口径偏乐观

## 输出目录说明

输出目录位置会改变结果解释：

- 输出到 SSD/tmpfs，更接近“读取核心性能”
- 输出到同一块 HDD，更接近总 I/O 压力

正式报告应说明输出目录所在磁盘和测试环境。

## 主维度单列读取

题目还要求支持主维度单列高效读取。当前仓库提供：

- `erwt3d_line`
- `erwt3d_bench_line`

它们用于功能验证和单列 benchmark，但不计入当前 60 分性能公式。
