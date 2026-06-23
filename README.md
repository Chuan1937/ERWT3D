# ERWT3D

三维空间数据高效读写库（HDD 优化）

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## 使用

```bash
./build/erwt3d_bench_contest --input data.erwt3d --output-dir out --hdd
./build/erwt3d_convert --input data.raw --output data.erwt3d --nx 801 --ny 2405 --nz 2501
./build/erwt3d_verify --raw data.raw --erwt3d data.erwt3d --nx 801 --ny 2405 --nz 2501
```

## HDD 性能

| 数据集 | T_composite |
|--------|-------------|
| 20GB (801×2405×2501) | 94.11s |
| 50GB (2001×2201×3000) | 223.43s |

## 文档

- [存储结构与算法](docs/design.md)
- [性能测试](docs/benchmark.md)
- [赛题说明](docs/competiton_guide.md)

## 许可

BSD 3-Clause License
