# RZFP Design Notes

RZFP = **R**elative-error-bounded **ZFP**.

## Why not the old tri-axis fixed-rate route?

The earlier experiment (`experiment/tri-axis-zfp-3copy`) stored three complete
X/Y/Z copies with ZFP fixed-rate (`rate=16 bpp`).  That produced:

- 1.677x storage (three copies + exception blocks)
- 10.4% exception blocks on 20GB due to near-zero values
- 20GB `T_composite` ≈ 46.84s

The three-copy overhead alone (~1.5x) consumed almost the entire storage
budget, leaving no room for exceptions.  Fixed-rate also cannot adapt the
absolute tolerance to the local magnitude, so small values immediately violate
the relative bound.

## Core idea

Keep **one single copy** of the 3D volume.  Each `4×4×4` leaf is compressed
independently with one of several candidate encodings:

- `ConstantZero` / `ConstantValue`
- ZFP fixed-accuracy with block-min tolerance → relative bound
- ZFP fixed-accuracy + exact exception points for zeros/subnormals/Inf/NaN
- ZFP fixed-precision
- Raw `float32` fallback

The final candidate is the smallest one that:

1. decodes back with **strict** pointwise relative error `< 0.001`, and
2. beats raw by at least `minimum_size_gain` (default 3%).

## From relative bound to absolute tolerance

For a leaf, let

```
m = min |x_i|   over all non-exception valid values
```

The internal relative bound is `0.00075`.  We request an absolute ZFP tolerance
of

```
tau = 0.00075 * m
```

and round down to the next power of two so ZFP's guarantee is strictly below
the contest bound:

```
requested_tolerance = 2^floor(log2(tau)) <= tau
```

If ZFP guarantees `|decoded - original| <= requested_tolerance`, then for every
non-exception value `x_i`:

```
|decoded - original| / |x_i| <= requested_tolerance / m <= 0.00075 < 0.001
```

## Mandatory exceptions

The following points are always stored exactly, never handed to ZFP:

- `original == 0.0f`
- `NaN` or `Inf`
- subnormal numbers

Reasons:

- Relative error is undefined at zero.
- Subnormals require impractically small absolute tolerances.
- Special floating-point values must preserve bit-level semantics.

Exceptions are patched back after ZFP decompression using a per-leaf bitmask.

## Why verify after every encode?

ZFP's accuracy/precision modes give statistical guarantees, not strict
per-point relative bounds.  Therefore every candidate is **decompressed and
verified** before it is accepted.  If no candidate passes, the leaf falls back
to raw `float32`.

## Why leaf-level reading helps random slices

A random X/Y/Z slice only touches a thin subset of each superblock.  In the old
superblock-based reader that meant reading almost an entire superblock (1 MiB)
for every leaf touched.  With RZFP each leaf is a small independent record,
so the reader can:

1. collect all unique `(sb, morton)` leaf references from all requested slices,
2. sort them by file offset,
3. merge nearby records into sequential HDD windows, and
4. decode only the touched leaves once and scatter to every output slice.

For high-coverage queries the reader still reads the variable payload region
mostly sequentially.

## Known limitations / future work

- Encode throughput in the probe is ~6–8 MB/s of valid points.  Full conversion
  of 50GB will be slow and needs further candidate-mode pruning.
- `RzfpReader` currently decodes leaves single-threaded after I/O.  A separate
  decode-thread pool is planned.
- Only `ZYX` (and `v05-yzx`) physical ordering is wired; other layouts need
  validation.
- Full 20GB/50GB conversion and contest benchmark on HDD have not been run
  yet (`NOT_BENCHMARKED`).
