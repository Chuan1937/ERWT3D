# Official Data Format Notes

Derived from measurement only. No official code or restricted content included.

## Small Dataset (small.dat / cup_3d_small.dat)

| Property | Value |
|----------|-------|
| Data type | float32 (32-bit single precision) |
| Storage order | X-Y-Z row-major (X varies fastest, then Y, then Z) |
| Dimensions | 801 × 2405 × 2501 |
| File size | 19,271,755,620 bytes (18.0 GB) |
| Z-slice (raw) | Contiguous 801×2405 = 7.7 MB per slice; fastest axis |
| X-slice (raw) | Strided across entire file; slowest axis |

## Big Dataset (big.dat / cup_3d_big.dat)

| Property | Value |
|----------|-------|
| Data type | float32 (32-bit single precision) |
| Storage order | X-Y-Z row-major |
| Dimensions | 2001 × 2201 × 3000 |
| File size | 52,850,412,000 bytes (49.2 GB) |
| Z-slice (raw) | Contiguous 2001×2201 = 17.6 MB per slice; fastest |
| X-slice (raw) | Strided across entire file; slowest |

## Competition Rules (derived)

- Storage ratio < 1.5x for full 20 storage points
- Single machine, single process, multi-thread allowed
- Memory configurable via --memory-limit-mb
- Performance score: (baseline_time / actual_composite_time) × 60
- actual_composite_time = combined X/Y/Z random + continuous read/write time
