# ERWT3D

三维空间数据高效读写库

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## 使用

```bash
./run_bench_hdd.sh    # HDD 测试
./run_bench_ssd.sh    # SSD 测试
./run_convert.sh      # 转换
./run_verify.sh       # 验证
```

脚本内容（可编辑调整参数）：

```bash
#!/bin/bash
erwt3d begin mytest
    erwt3d bench_hdd input=data.erwt3d output=/tmp/out
erwt3d end
```

## 参数

| 参数 | 说明 |
|------|------|
| `input=` | 输入文件 |
| `output=` | 输出目录/文件 |
| `nx=` `ny=` `nz=` | 数据维度 |
| `random=` | 随机切片数 (默认 100) |
| `continuous=` | 连续切片数 (默认 10) |
| `threads=` | 线程数 |
| `memory=` | 内存限制 (MB) |

## 性能

| 环境 | T_total (20G) |
|------|---------------|
| SSD | 73ms |
| HDD | 877ms |

## 文档

- [存储结构](docs/design.md)
- [索引原理](docs/index.md)
- [算法实现](docs/implementation.md)
- [性能测试](docs/benchmark.md)

## 许可

BSD 3-Clause License
