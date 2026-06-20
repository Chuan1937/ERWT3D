# ERWT3D

三维空间数据高效读写库

## 构建

```bash
make build
make install    # 输出 PATH 配置，加入 ~/.bashrc
```

## 使用

```bash
erwt3d bench_hdd  input=data.erwt3d output=/tmp/out
erwt3d bench_ssd  input=data.erwt3d output=/tmp/out
erwt3d convert    input=data.raw output=data.erwt3d nx=801 ny=2405 nz=2501
erwt3d verify     raw=data.raw erwt3d=data.erwt3d nx=801 ny=2405 nz=2501
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
