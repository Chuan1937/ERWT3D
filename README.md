# ERWT3D

三维空间数据高效读写库，面向赛题2“三维空间数据的高效读写”。核心存储布局仍是单文件、Superblock + Leaf、Morton Z-order；本次收口重点放在更严格的 benchmark、verify、脚本和文档口径。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## 严格比赛模拟

赛题性能口径仍为：

```text
T_composite = (T_x_random + T_y_random + T_z_random + T_x_continuous + T_y_continuous + T_z_continuous) / 6
```

预处理时间不计入性能分，但预处理后全部必要文件大小计入存储分。推荐先做相对误差验证，再做严格 benchmark：

```bash
./build/erwt3d_verify \
  --raw data.dat \
  --erwt3d data.erwt3d \
  --nx NX --ny NY --nz NZ \
  --samples 100000 \
  --seed 20260511 \
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

`--timing-mode strict` 是推荐正式口径，计时覆盖 output file 的 open/create、切片读取、解码/重排、raw 写出和 close。`--timing-mode fast` 只适合调试核心读取性能，不适合正式报告。

## 关键工具

| 二进制 | 用途 |
|--------|------|
| `erwt3d_convert` | Raw ↔ ERWT3D 转换 |
| `erwt3d_bench_contest` | 赛题2 六组 benchmark |
| `erwt3d_bench` | 通用 benchmark |
| `erwt3d_verify` | 正确性验证，默认按相对误差判定 |
| `erwt3d_slice` | 单切片读取 |
| `erwt3d_line` | 主维度单列读取 |
| `erwt3d_bench_line` | 主维度单列 benchmark，不计入 60 分公式 |
| `erwt3d_info` | 文件信息查看 |
| `erwt3d_precompute_x` | 可选 X-plane 预计算 |

## 验证与计时说明

`erwt3d_verify` 现在默认按赛题要求使用相对误差判据：

- 官方否决项是单点相对误差 `< 0.001`
- 对参考值接近 0 的点，默认使用 `--zero-abs-tol 1e-6` 保护
- 采样验证使用 `std::mt19937_64`，可通过 `--seed` 复现
- 如果要完全严格地对所有点都按相对误差处理，可加 `--strict-relative`

`erwt3d_bench_contest` 的连续切片起点默认是 `--continuous-start random`，并由 `--seed` 控制复现。`--storage-path` 支持统计单文件，也支持递归统计整个预处理目录，更接近“预处理后全部文件都计入存储分”的比赛口径；如果用户显式传入 `--storage-path` 但路径不存在、不可访问或递归统计失败，程序会直接报错退出，避免误算存储分。

`--repeats` 默认仍为 `1`。当 `repeats > 1` 时，`T_composite` 仍按每组最小值计算以保持兼容，但 CSV 和终端会同时输出 min/mean/median/max，并提示该结果偏乐观。

## 输出目录建议

输出目录位置会直接影响结果：

- 输出目录放在 SSD 或 tmpfs，更偏向读取算法本身
- 输出目录放在同一块 HDD，更接近比赛中的总 I/O 压力
- 正式报告应明确写出 output 目录所在磁盘

## 脚本

| 脚本 | 用途 |
|------|------|
| `scripts/benchmark_contest_strict.sh` | 严格比赛模拟 |
| `scripts/benchmark.sh` | 日常调参 benchmark，默认 `fast` |
| `scripts/verify_contest.sh` | 赛题口径验证 |
| `scripts/verify.sh` | 常用验证脚本 |
| `scripts/bench_mem_sweep.sh` | 内存限制扫描 |

## 文档

- [性能测试](docs/benchmark.md)
- [赛题说明](docs/competiton_guide.md)
- [存储结构与算法](docs/design.md)
