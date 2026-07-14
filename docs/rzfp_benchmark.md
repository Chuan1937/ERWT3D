# RZFP Benchmark Results

All times are wall-clock read + write on the local test machine unless
otherwise noted.

## Probe results

Probe uses 1 000 000 stratified leaves, internal relative bound `0.00075`,
contest bound `0.001`, `ZYX` physical order.

### 64³ synthetic random [0,1]

```text
violation_count:        0
max_relative_error:     0.000999
projected_file_ratio:   0.610x
raw_fallback_ratio:     5.5%
```

### 17×19×21 boundary test

```text
violation_count:        0
max_relative_error:     0.000999
projected_file_ratio:   0.790x
raw_fallback_ratio:     15.3%
```

### 20GB real (`801×2405×2501`)

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

### 50GB real (`2001×2201×3000`)

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

## End-to-end small volume

| Volume | Convert ratio | Full verify |
|--------|---------------|-------------|
| 64³    | 0.586x        | passed      |
| 17³    | 1.000x        | passed      |

## Contest benchmark (small volume, 100 random / 10 continuous)

Machine: local SSD, single thread, 512 MB memory limit.

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

## NOT_BENCHMARKED

The following have not been run yet:

- Full 20GB RZFP conversion
- Full 50GB RZFP conversion
- 20GB contest benchmark on HDD
- 50GB contest benchmark on HDD
- A-B-A comparison against v0.5.1/v0.6.0 LZ4 baseline

These require the slow conversion step to complete first and must be run on
the D-drive HDD as required by the project rules.
