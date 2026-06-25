# ERWT3D

三维空间数据高效读写库（HDD 优化）

赛题2 - 三维空间数据的高效读写

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## 快速开始

```bash
# 转换
./build/erwt3d_convert --input data.raw --output data.erwt3d \
    --nx 801 --ny 2405 --nz 2501 --threads 8 --memory-limit-mb 4096

# 赛题 benchmark
./build/erwt3d_bench_contest --input data.erwt3d --output-dir out --hdd

# 验证正确性
./build/erwt3d_verify --raw data.raw --erwt3d data.erwt3d \
    --nx 801 --ny 2405 --nz 2501 --samples 100000
```

## 核心二进制

| 二进制 | 用途 |
|--------|------|
| `erwt3d_convert` | Raw ↔ ERWT3D 格式转换 |
| `erwt3d_bench_contest` | 赛题标准 benchmark（6 组测试） |
| `erwt3d_bench` | 通用 benchmark，支持全部参数 |
| `erwt3d_slice` | 单切片读取 |
| `erwt3d_verify` | 正确性验证 |
| `erwt3d_info` | 文件信息查看 |
| `erwt3d_precompute_x` | X-plane 预计算（可选加速） |

## HDD 性能

D 盘机械硬盘实测（WSL 9p，`--hdd` 模式，4GB 内存限制）：

| 数据集 | T_composite | 存储比 |
|--------|------------|--------|
| 20GB (801×2405×2501) | 34.42s | 1.075x |
| 50GB (2001×2201×3000) | 87.67s | 1.044x |

带宽利用率 93.6%，接近 HDD 物理极限。

## 存储设计

- 两级层次：Superblock (64³ = 1MB) → Leaf (4³ = 256B)
- Morton Z-order 物理布局，三轴访问均衡
- 公式计算偏移，无需索引表
- 128MB 读窗口 + 1MB gap 容忍 + 文件偏移排序
- 单线程顺序 pread + readahead 内核预取

## 脚本

| 脚本 | 功能 |
|------|------|
| `scripts/benchmark.sh` | 赛题 benchmark |
| `scripts/bench_mem_sweep.sh` | 内存限制扫描 |
| `scripts/bench_single.sh` | 单切片延迟测试 |
| `scripts/convert.sh` | 转换 + 验证 |
| `scripts/verify.sh` | 正确性验证 |

## 文档

- [存储结构与算法](docs/design.md)
- [性能测试](docs/benchmark.md)
- [赛题说明](docs/competiton_guide.md)
- [变更日志](CHANGELOG.md)

## 许可

BSD 3-Clause License
