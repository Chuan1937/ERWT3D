# HDD 性能测试

## 环境

- D盘机械硬盘，WSL 9p 挂载
- 配置：`--hdd`（128MB 读窗口，1MB gap，单线程）

## 20GB（801×2405×2501）

存储比 1.075x → 20/20 分

| 测试项 | 总时间 | 每片 |
|--------|--------|------|
| X random (100片) | 307.20s | 3.07s |
| Y random (100片) | 127.39s | 1.27s |
| Z random (100片) | 120.02s | 1.20s |
| X continuous (10片) | 6.35s | 0.64s |
| Y continuous (10片) | 1.85s | 0.18s |
| Z continuous (10片) | 1.83s | 0.18s |
| **T_composite** | **94.11s** | |

## 50GB（2001×2201×3000）

存储比 1.044x → 20/20 分

| 测试项 | 总时间 | 每片 |
|--------|--------|------|
| X random (100片) | 640.44s | 6.40s |
| Y random (100片) | 379.19s | 3.79s |
| Z random (100片) | 302.88s | 3.03s |
| X continuous (10片) | 7.62s | 0.76s |
| Y continuous (10片) | 6.01s | 0.60s |
| Z continuous (10片) | 4.46s | 0.45s |
| **T_composite** | **223.43s** | |

## 瓶颈

X 切片最慢，触及 gridY×gridZ 个分散 superblock（间隔 13-32MB），无法合并读窗口。Panel stride=4 只命中 25% 索引。

## 推荐命令

```bash
./build/erwt3d_bench_contest --input data.erwt3d --output-dir out --hdd
```
