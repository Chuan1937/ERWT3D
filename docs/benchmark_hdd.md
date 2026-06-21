# HDD 性能测试

## 测试环境

| 项目 | 配置 |
|------|------|
| CPU | Intel i7-13700F, 24 线程 |
| RAM | 64 GB |
| HDD | 数据盘 F/G |

## 评分公式

```
T_composite = (T_xr + T_yr + T_zr + T_xc + T_yc + T_zc) / 6
得分 = (基准时间 / T_composite) × 60
```

## 关键发现

HDD 必须单线程顺序读取，多线程并发会导致磁头跳动。

## 测试结果

### 20G 数据集 (801×2405×2501)

#### 模式对比

| 模式 | X random | Y random | Z random | T_total |
|------|----------|----------|----------|---------|
| 逐切片读取 | 4673ms | 2443ms | 2637ms | ~3200ms |
| **Batch planner** | **927ms** | **1404ms** | **1263ms** | **877ms** |

## 推荐命令

```bash
erwt3d bench_hdd input=data.erwt3d output=/tmp/out
```

`--hdd` 自动设置: 单线程、4GB 内存、顺序读取、批量合并。
