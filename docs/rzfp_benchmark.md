# RZFP 基准测试结果

除非特别说明，所有时间均为本地测试机上的 wall-clock 读取 + 写入时间。

## Probe 结果

Probe 使用 1 000 000 个分层采样 Leaf，内部相对误差界 `0.00075`，比赛误差界 `0.001`，物理顺序 `ZYX`。

### 64³ 合成随机 [0,1]

```text
violation_count:        0
max_relative_error:     0.000999
projected_file_ratio:   0.610x
raw_fallback_ratio:     5.5%
```

### 17×19×21 边界测试

```text
violation_count:        0
max_relative_error:     0.000999
projected_file_ratio:   0.790x
raw_fallback_ratio:     15.3%
```

### 20GB 真实数据（`801×2405×2501`）

```text
violation_count:        0
max_relative_error:     0.000999
projected_file_ratio:   0.878x
raw_fallback_ratio:     30.8%
codec_distribution:
  RawFloat32:            30.8%
  ZfpAccuracyExceptions: 69.1%
  ZfpPrecision:          <0.1%
```

### 50GB 真实数据（`2001×2201×3000`）

```text
violation_count:        0
max_relative_error:     0.001000
projected_file_ratio:   0.843x
raw_fallback_ratio:     23.8%
codec_distribution:
  RawFloat32:            23.8%
  ZfpAccuracyExceptions: 55.1%
  ZfpPrecision:          18.9%
  ZfpAccuracy:           2.2%
```

## 小规模端到端

| 体积 | 转换后存储比 | 完整校验 |
|------|-------------|----------|
| 64³  | 0.586x      | 通过     |
| 17³  | 1.000x      | 通过     |

## 比赛基准（小规模，100 随机 / 10 连续）

机器：本地 SSD，单线程，512 MB 内存限制。

```text
X random:     0.0085s
Y random:     0.0073s
Z random:     0.0077s
X continuous: 0.0016s
Y continuous: 0.0015s
Z continuous: 0.0017s
T_composite:  0.0047s
storage_ratio: 0.586x
```

## HDD 比赛基准

机器：D 盘 HDD，单 I/O 线程，8 个解码线程，4 GB 内存限制，512 MB 读窗口，8 MB 最大 gap，策略 `auto`。

### 20GB 真实数据（`801×2405×2501`）

格式：RZFP 主文件 + 2D X-plane sidecar（stride = 1）。

```text
T_x_random:     ~15s
T_y_random:     ~?
T_z_random:     ~?
T_x_continuous: ~?
T_y_continuous: ~?
T_z_continuous: ~?
T_composite:    23.80s
storage_ratio:  1.369x
```

X-plane sidecar 是 X 随机访问最大的收益点；20GB 数据集的 YZ 平面空间相关性好，sidecar 压缩率可行。

### 50GB 真实数据（`2001×2201×3000`）

格式：仅 RZFP 主文件，不使用 X-plane sidecar。

```text
T_x_random:     ~?
T_y_random:     ~?
T_z_random:     ~?
T_x_continuous: ~?
T_y_continuous: ~?
T_z_continuous: ~?
T_composite:    83.58s
storage_ratio:  0.804x
```

50GB 数据集不适合加 sidecar：sidecar 压缩率仅约 0.979x，16 GB 的 sidecar 会污染 page cache，反而降低性能。关键改进是读窗口从 128 MB/2 MB 提升到 512 MB/8 MB（此前同配置约 123s）。

> **注意：** 83.58s 是当前最佳测量值，正式基准需要补充多轮冷缓存与热缓存重复性测试。该工作已在 PR #50 中跟踪。

## 存储格式建议

| 数据集 | 推荐格式 | 原因 |
|--------|----------|------|
| 20GB   | LZ4 + 2D X-plane sidecar | sidecar 在该卷上压缩效果好，读取更快 |
| 50GB   | 仅 RZFP 主文件 | 存储比已低于 1.0x，且当前 Reader 成绩最快 |

## 尚未完成的基准工作

- 83.58s 的冷缓存 vs 热缓存可重复性研究
- 与 v0.5.1/v0.6.0 LZ4 基线在同一 HDD 上的 A-B-A 对比
- 20GB sidecar 方案下 Y/Z 随机与所有连续组的单组时间
- 50GB 无 sidecar 方案下六个轴/模式的单组时间

所有全量基准必须按项目规则在 D 盘 HDD 上运行。
