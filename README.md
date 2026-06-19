# ERWT3D

三维空间数据高效读写库

## 特性

- **无冗余存储**: Morton 序物理布局，X/Y/Z 三轴访问均衡
- **多线程 I/O**: 线程池并行 pread
- **内存可控**: `--memory-limit-mb` 限制内存使用
- **HDD 优化**: 大读窗口 + gap 容忍 + 批量合并

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## 工具

| 工具 | 用途 |
|------|------|
| `erwt3d_info` | 显示文件信息 |
| `erwt3d_convert` | RAW ↔ ERWT3D 转换 |
| `erwt3d_slice` | 读取切片 |
| `erwt3d_line` | 读取单行 |
| `erwt3d_verify` | 正确性验证 |
| `erwt3d_bench` | 性能测试 |
| `erwt3d_bench_contest` | 赛题评分测试 |

## 快速开始

```bash
# 转换
./build/erwt3d_convert --input data.raw --output data.erwt3d --nx 801 --ny 2405 --nz 2501

# 测试
./build/erwt3d_bench_contest --input data.erwt3d --output-dir out --threads 8 --io-backend sb --sb-parallel-mode parallel-read

# 验证
./build/erwt3d_verify --raw data.raw --erwt3d data.erwt3d --nx 801 --ny 2405 --nz 2501
```

## 文档

- [存储结构](docs/design.md)
- [索引原理](docs/index.md)
- [算法实现](docs/implementation.md)
- [性能测试](docs/benchmark.md)

## 许可

BSD 3-Clause License
